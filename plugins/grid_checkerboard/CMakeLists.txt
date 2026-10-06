# Grid / Checkerboard: optional effect plugin (Effects > Render > Grid /
# Checkerboard). Builds <build>/plugins/out/grid_checkerboard/ with
# grid_checkerboard<suffix>, this folder's README.md and screenshot.png;
# copy that folder into a paint.c plugins folder to install it (README.md).
pc_add_plugin(grid_checkerboard fxm_grid_checkerboard.c)
