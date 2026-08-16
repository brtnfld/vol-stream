# Live heatmap of rd_monitor's stream. Run from the same directory
# rd_monitor is writing reaction_diffusion_frame.dat into:
#
#   gnuplot examples/reaction_diffusion/plot_live.gnuplot
#
# `reread` re-executes this script every REFRESH_SEC -- an ordinary poll
# loop, not a gnuplot animation feature. rename()'s atomicity (see
# rd_write_frame() in rd_monitor.c) is what keeps this from ever reading a
# half-written frame. No fixed `cbrange`: V has no fixed physical bound the
# way heat_diffusion's temperature does, so this autoscales every frame,
# same as rd_monitor's own ASCII contrast stretch.

REFRESH_SEC = 0.2

set title 'reaction_diffusion -- live'
unset key
set size ratio -1
set view map
set cblabel 'V'
set palette gray

plot 'reaction_diffusion_frame.dat' matrix with image

pause REFRESH_SEC
reread
