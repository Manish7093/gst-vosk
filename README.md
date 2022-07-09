# gst-vosk
Gstreamer plugin for VOSK voice recognition engine

# Dependencies
- Gio
- meson (0.56.0)
- Gstreamer (> 1.20)

# Building this plugin
To install the plugin in /usr (where most distributions put Gstreamer plugins):
meson setup builddir
meson compile --prefix=/usr
meson install
