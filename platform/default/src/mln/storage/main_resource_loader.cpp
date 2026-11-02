#include <mln/actor/actor.hpp>
#include <mln/actor/scheduler.hpp>
#include <mln/platform/settings.hpp>
#include <mln/storage/file_source_manager.hpp>
#include <mln/storage/file_source_request.hpp>
#include <mln/storage/main_resource_loader.hpp>
#include <mln/storage/resource.hpp>
#include <mln/storage/resource_options.hpp>
#include <mln/storage/response.hpp>
#include <mln/util/async_request.hpp>
#include <mln/util/client_options.hpp>
#include <mln/util/image.hpp>
#include <mln/util/platform.hpp>
#include <mln/util/stopwatch.hpp>
#include <mln/util/thread.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// GPS Tuner: elevation tiles generated in-process by the host app ("gtdem://{z}/{x}/{y}").
// The provider fills size * size elevations in meters (row-major, north to south) for the
// given XYZ tile and returns non-zero on success; returning zero means no data for the tile.
using GpstDemProviderFn = int (*)(void* context, int z, int x, int y, int size, float* elevations);

namespace {

std::atomic<GpstDemProviderFn> gpstDemProvider{nullptr};
std::atomic<void*> gpstDemProviderContext{nullptr};

} // namespace

extern "C" __attribute__((visibility("default"))) void mln_gpst_set_dem_provider(GpstDemProviderFn provider,
                                                                                  void* context) {
    gpstDemProviderContext.store(context);
    gpstDemProvider.store(provider);
}

namespace mln {

namespace {

constexpr const char* gpstDemScheme = "gtdem://";
constexpr int gpstDemTileSize = 256;

bool isGpstDemRequest(const Resource& resource) {
    return resource.url.rfind(gpstDemScheme, 0) == 0;
}

// Encodes the elevations as a Terrarium PNG: height = (R * 256 + G + B / 256) - 32768.
Response loadGpstDemTile(const Resource& resource) {
    Response response;
    const auto provider = gpstDemProvider.load();
    if (!provider || !resource.tileData) {
        response.noContent = true;
        return response;
    }

    const auto& tile = *resource.tileData;
    std::vector<float> elevations(gpstDemTileSize * gpstDemTileSize);
    if (!provider(gpstDemProviderContext.load(), tile.z, tile.x, tile.y, gpstDemTileSize, elevations.data())) {
        response.noContent = true;
        return response;
    }

    PremultipliedImage image({gpstDemTileSize, gpstDemTileSize});
    for (std::size_t i = 0; i < elevations.size(); ++i) {
        const double value = std::clamp(static_cast<double>(elevations[i]) + 32768.0, 0.0, 65535.99);
        const double whole = std::floor(value);
        const auto integer = static_cast<uint32_t>(whole);
        image.data[i * 4 + 0] = static_cast<uint8_t>(integer >> 8);
        image.data[i * 4 + 1] = static_cast<uint8_t>(integer & 0xff);
        image.data[i * 4 + 2] = static_cast<uint8_t>((value - whole) * 256.0);
        image.data[i * 4 + 3] = 255;
    }

    response.data = std::make_shared<std::string>(encodePNG(image));
    response.expires = Timestamp::max();
    return response;
}

// Generates the elevation tiles on their own thread, so they never hold up the other requests
// of the resource loader thread (vector tiles). Newest requests first: those are the tiles the
// current view needs. Requests cancelled before they are reached (tiles that left the view)
// are dropped without generating them.
class GpstDemWorker {
public:
    using Callback = std::function<void(const Response&)>;

    static GpstDemWorker& get() {
        static GpstDemWorker worker;
        return worker;
    }

    std::shared_ptr<std::atomic<bool>> post(const Resource& resource, Callback callback) {
        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        {
            std::lock_guard<std::mutex> lock(mutex);
            jobs.push_back(std::make_unique<Job>(Job{resource, std::move(callback), cancelled}));
        }
        condition.notify_one();
        return cancelled;
    }

    ~GpstDemWorker() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        condition.notify_one();
        thread.join();
    }

private:
    struct Job {
        Resource resource;
        Callback callback;
        std::shared_ptr<std::atomic<bool>> cancelled;
    };

    GpstDemWorker()
        : thread([this] { run(); }) {}

    void run() {
        platform::setCurrentThreadName("GpstDemWorker");
        while (true) {
            std::unique_ptr<Job> job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock, [this] { return stopping || !jobs.empty(); });
                if (stopping) return;
                job = std::move(jobs.back());
                jobs.pop_back();
            }
            if (job->cancelled->load()) continue;
            Response response = loadGpstDemTile(job->resource);
            if (!job->cancelled->load()) job->callback(response);
        }
    }

    std::mutex mutex;
    std::condition_variable condition;
    std::deque<std::unique_ptr<Job>> jobs;
    bool stopping = false;
    std::thread thread;
};

class GpstDemRequest : public AsyncRequest {
public:
    explicit GpstDemRequest(std::shared_ptr<std::atomic<bool>> cancelled_)
        : cancelled(std::move(cancelled_)) {}
    ~GpstDemRequest() override { cancelled->store(true); }

private:
    std::shared_ptr<std::atomic<bool>> cancelled;
};

} // namespace

class MainResourceLoaderThread {
public:
    MainResourceLoaderThread(std::shared_ptr<FileSource> assetFileSource_,
                             std::shared_ptr<FileSource> databaseFileSource_,
                             std::shared_ptr<FileSource> localFileSource_,
                             std::shared_ptr<FileSource> onlineFileSource_,
                             std::shared_ptr<FileSource> mbtilesFileSource_,
                             std::shared_ptr<FileSource> pmtilesFileSource_)
        : assetFileSource(std::move(assetFileSource_)),
          databaseFileSource(std::move(databaseFileSource_)),
          localFileSource(std::move(localFileSource_)),
          onlineFileSource(std::move(onlineFileSource_)),
          mbtilesFileSource(std::move(mbtilesFileSource_)),
          pmtilesFileSource(std::move(pmtilesFileSource_)) {}

    void request(AsyncRequest* req, const Resource& resource, const ActorRef<FileSourceRequest>& ref) {
        auto callback = [ref](const Response& res) {
            ref.invoke(&FileSourceRequest::setResponse, res);
        };

        if (isGpstDemRequest(resource)) {
            tasks[req] = std::make_unique<GpstDemRequest>(GpstDemWorker::get().post(resource, callback));
            return;
        }

        auto requestFromNetwork = [=, this](const Resource& res,
                                            std::unique_ptr<AsyncRequest> parent) -> std::unique_ptr<AsyncRequest> {
            if (!onlineFileSource || !onlineFileSource->canRequest(resource)) {
                return parent;
            }

            // Keep parent request alive while chained request is being processed.
            std::shared_ptr<AsyncRequest> parentKeepAlive = std::move(parent);

            MBGL_TIMING_START(watch);
            return onlineFileSource->request(res, [=, ptr = parentKeepAlive, this](const Response& response) {
                if (databaseFileSource) {
                    databaseFileSource->forward(res, response, nullptr);
                }
                if (res.kind == Resource::Kind::Tile) {
                    // onlineResponse.data will be null if data not modified
                    MBGL_TIMING_FINISH(watch,
                                       " Action: " << "Requesting,"
                                                   << " URL: " << res.url.c_str() << " Size: "
                                                   << (response.data != nullptr ? response.data->size() : 0) << "B,"
                                                   << " Time")
                }
                callback(response);
            });
        };

        // Initial tasksSize is used to check whether any of
        // the sources were able to request a resource.
        const std::size_t tasksSize = tasks.size();

        // Waterfall resource request processing and return early once resource was requested.
        if (assetFileSource && assetFileSource->canRequest(resource)) {
            // Asset request
            tasks[req] = assetFileSource->request(resource, callback);
        } else if (mbtilesFileSource && mbtilesFileSource->canRequest(resource)) {
            // Local file request
            tasks[req] = mbtilesFileSource->request(resource, callback);
        } else if (pmtilesFileSource && pmtilesFileSource->canRequest(resource)) {
            // Local file request
            tasks[req] = pmtilesFileSource->request(resource, callback);
        } else if (localFileSource && localFileSource->canRequest(resource)) {
            // Local file request
            tasks[req] = localFileSource->request(resource, callback);
        } else if (databaseFileSource && databaseFileSource->canRequest(resource)) {
            // Try cache only request if needed.
            if (resource.loadingMethod == Resource::LoadingMethod::CacheOnly) {
                tasks[req] = databaseFileSource->request(resource, callback);
            } else {
                // Cache request with fallback to network with cache control
                tasks[req] = databaseFileSource->request(resource, [=, this](const Response& response) {
                    Resource res = resource;

                    // Resource is in the cache
                    if (!response.noContent) {
                        if (response.isUsable()) {
                            callback(response);
                            // Set the priority of existing resource to low if it's expired but usable.
                            res.setPriority(Resource::Priority::Low);
                        } else {
                            // Set prior data only if it was not returned to
                            // the requester. Once we get 304 response from
                            // the network, we will forward response to the
                            // requester.
                            res.priorData = response.data;
                        }

                        // Copy response fields for cache control request
                        res.priorModified = response.modified;
                        res.priorExpires = response.expires;
                        res.priorEtag = response.etag;
                    }

                    tasks[req] = requestFromNetwork(res, std::move(tasks[req]));
                });
            }
        } else if (auto networkReq = requestFromNetwork(resource, nullptr)) {
            // Get from the online file source
            tasks[req] = std::move(networkReq);
        }

        // If no new tasks were added, notify client that request cannot be processed.
        if (tasks.size() == tasksSize) {
            Response response;
            response.noContent = true;
            response.error = std::make_unique<Response::Error>(Response::Error::Reason::Other,
                                                               "Unsupported resource request.");
            callback(response);
        }
    }

    void cancel(AsyncRequest* req) {
        assert(req);
        tasks.erase(req);
    }

private:
    const std::shared_ptr<FileSource> assetFileSource;
    const std::shared_ptr<FileSource> databaseFileSource;
    const std::shared_ptr<FileSource> localFileSource;
    const std::shared_ptr<FileSource> onlineFileSource;
    const std::shared_ptr<FileSource> mbtilesFileSource;
    const std::shared_ptr<FileSource> pmtilesFileSource;
    std::map<AsyncRequest*, std::unique_ptr<AsyncRequest>> tasks;
};

class MainResourceLoader::Impl {
public:
    Impl(const ResourceOptions& resourceOptions_,
         const ClientOptions& clientOptions_,
         std::shared_ptr<FileSource> assetFileSource_,
         std::shared_ptr<FileSource> databaseFileSource_,
         std::shared_ptr<FileSource> localFileSource_,
         std::shared_ptr<FileSource> onlineFileSource_,
         std::shared_ptr<FileSource> mbtilesFileSource_,
         std::shared_ptr<FileSource> pmtilesFileSource_)
        : assetFileSource(std::move(assetFileSource_)),
          databaseFileSource(std::move(databaseFileSource_)),
          localFileSource(std::move(localFileSource_)),
          onlineFileSource(std::move(onlineFileSource_)),
          mbtilesFileSource(std::move(mbtilesFileSource_)),
          pmtilesFileSource(std::move(pmtilesFileSource_)),
          supportsCacheOnlyRequests_(bool(databaseFileSource)),
          thread(std::make_unique<util::Thread<MainResourceLoaderThread>>(
              util::makeThreadPrioritySetter(platform::EXPERIMENTAL_THREAD_PRIORITY_WORKER),
              "ResourceLoaderThread",
              assetFileSource,
              databaseFileSource,
              localFileSource,
              onlineFileSource,
              mbtilesFileSource,
              pmtilesFileSource)),
          resourceOptions(resourceOptions_.clone()),
          clientOptions(clientOptions_.clone()) {}

    std::unique_ptr<AsyncRequest> request(const Resource& resource, Callback callback) {
        auto req = std::make_unique<FileSourceRequest>(std::move(callback));

        req->onCancel([actorRef = thread->actor(), req = req.get()]() {
            actorRef.invoke(&MainResourceLoaderThread::cancel, req);
        });
        thread->actor().invoke(&MainResourceLoaderThread::request, req.get(), resource, req->actor());
        return req;
    }

    bool canRequest(const Resource& resource) const {
        return isGpstDemRequest(resource) || (assetFileSource && assetFileSource->canRequest(resource)) ||
               (localFileSource && localFileSource->canRequest(resource)) ||
               (databaseFileSource && databaseFileSource->canRequest(resource)) ||
               (onlineFileSource && onlineFileSource->canRequest(resource)) ||
               (mbtilesFileSource && mbtilesFileSource->canRequest(resource)) ||
               (pmtilesFileSource && pmtilesFileSource->canRequest(resource));
    }

    bool supportsCacheOnlyRequests() const { return supportsCacheOnlyRequests_; }

    void pause() { thread->pause(); }

    void resume() { thread->resume(); }

    void setResourceOptions(ResourceOptions options) {
        std::scoped_lock lock(resourceOptionsMutex);
        resourceOptions = options;
        assetFileSource->setResourceOptions(options.clone());
        databaseFileSource->setResourceOptions(options.clone());
        localFileSource->setResourceOptions(options.clone());
        onlineFileSource->setResourceOptions(options.clone());
        mbtilesFileSource->setResourceOptions(options.clone());
        pmtilesFileSource->setResourceOptions(options.clone());
    }

    ResourceOptions getResourceOptions() {
        std::scoped_lock lock(resourceOptionsMutex);
        return resourceOptions.clone();
    }

    void setClientOptions(ClientOptions options) {
        std::scoped_lock lock(clientOptionsMutex);
        clientOptions = options;
        assetFileSource->setClientOptions(options.clone());
        databaseFileSource->setClientOptions(options.clone());
        localFileSource->setClientOptions(options.clone());
        onlineFileSource->setClientOptions(options.clone());
        mbtilesFileSource->setClientOptions(options.clone());
        pmtilesFileSource->setClientOptions(options.clone());
    }

    ClientOptions getClientOptions() {
        std::scoped_lock lock(clientOptionsMutex);
        return clientOptions.clone();
    }

private:
    const std::shared_ptr<FileSource> assetFileSource;
    const std::shared_ptr<FileSource> databaseFileSource;
    const std::shared_ptr<FileSource> localFileSource;
    const std::shared_ptr<FileSource> onlineFileSource;
    const std::shared_ptr<FileSource> mbtilesFileSource;
    const std::shared_ptr<FileSource> pmtilesFileSource;
    const bool supportsCacheOnlyRequests_;
    const std::unique_ptr<util::Thread<MainResourceLoaderThread>> thread;
    mutable std::mutex resourceOptionsMutex;
    ResourceOptions resourceOptions;
    mutable std::mutex clientOptionsMutex;
    ClientOptions clientOptions;
};

MainResourceLoader::MainResourceLoader(const ResourceOptions& resourceOptions, const ClientOptions& clientOptions)
    : impl(std::make_unique<Impl>(
          resourceOptions.clone(),
          clientOptions.clone(),
          FileSourceManager::get()->getFileSource(FileSourceType::Asset, resourceOptions, clientOptions),
          FileSourceManager::get()->getFileSource(FileSourceType::Database, resourceOptions, clientOptions),
          FileSourceManager::get()->getFileSource(FileSourceType::FileSystem, resourceOptions, clientOptions),
          FileSourceManager::get()->getFileSource(FileSourceType::Network, resourceOptions, clientOptions),
          FileSourceManager::get()->getFileSource(FileSourceType::Mbtiles, resourceOptions, clientOptions),
          FileSourceManager::get()->getFileSource(FileSourceType::Pmtiles, resourceOptions, clientOptions))) {}

MainResourceLoader::~MainResourceLoader() = default;

bool MainResourceLoader::supportsCacheOnlyRequests() const {
    return impl->supportsCacheOnlyRequests();
}

std::unique_ptr<AsyncRequest> MainResourceLoader::request(const Resource& resource, Callback callback) {
    return impl->request(resource, std::move(callback));
}

bool MainResourceLoader::canRequest(const Resource& resource) const {
    return impl->canRequest(resource);
}

void MainResourceLoader::pause() {
    impl->pause();
}

void MainResourceLoader::resume() {
    impl->resume();
}

void MainResourceLoader::setResourceOptions(ResourceOptions options) {
    impl->setResourceOptions(options.clone());
}

ResourceOptions MainResourceLoader::getResourceOptions() {
    return impl->getResourceOptions();
}

void MainResourceLoader::setClientOptions(ClientOptions options) {
    impl->setClientOptions(options.clone());
}

ClientOptions MainResourceLoader::getClientOptions() {
    return impl->getClientOptions();
}

} // namespace mln
