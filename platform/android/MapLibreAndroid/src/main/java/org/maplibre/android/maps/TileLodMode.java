package org.maplibre.android.maps;

/**
 * How the zoom level of the tiles covering the view is chosen.
 * Mirrors mln::TileLodMode.
 */
public enum TileLodMode {

  /**
   * One zoom level for the whole view; the zoom only decreases with distance above the
   * pitch threshold (see {@link MapLibreMap#setTileLodPitchThreshold(double)}).
   */
  DEFAULT,

  /**
   * Distance-based zoom at every pitch.
   */
  DISTANCE,

  /**
   * Per-tile zoom from the field of view and a tile-count budget: finer near the camera,
   * coarser toward the horizon, as maplibre-gl-js does whenever terrain is present.
   */
  ADAPTIVE
}
