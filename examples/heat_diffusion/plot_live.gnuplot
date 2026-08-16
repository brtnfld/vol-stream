# Live heatmap of heat_monitor's stream. Run from the same directory
# heat_monitor is writing heat_diffusion_frame.dat into:
#
#   gnuplot examples/heat_diffusion/plot_live.gnuplot
#
# `reread` re-executes this script every REFRESH_SEC, so it picks up
# whatever heat_monitor last wrote -- an ordinary poll loop, not a gnuplot
# animation feature. rename()'s atomicity (see heat_write_frame() in
# heat_monitor.c) is what keeps this from ever reading a half-written frame.

REFRESH_SEC = 0.2

set title 'heat_diffusion -- live'
unset key
set size ratio -1
set view map
set cbrange [0:100]
set cblabel 'temperature'
set palette defined (0 '#08306b', 0.35 '#4292c6', 0.6 '#fdae6b', 0.8 '#e6550d', 1 '#a50026')

plot 'heat_diffusion_frame.dat' matrix with image

pause REFRESH_SEC
reread
