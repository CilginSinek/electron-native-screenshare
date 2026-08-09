{
  "targets": [
    {
      "target_name": "electron_native_screenshare",
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")"
      ],
      "dependencies": [
        "<!(node -p \"require('node-addon-api').gyp\")"
      ],
      "defines": [
        "NAPI_DISABLE_CPP_EXCEPTIONS"
      ],
      "conditions": [
        ["OS==\"win\"", {
          "sources": [
            "src/win/addon.cpp",
            "src/win/wasapi_capture.cpp"
          ],
          "defines": [
            "_WIN32_WINNT=0x0A00",
            "NTDDI_VERSION=0x0A00000A"
          ],
          "libraries": [
            "-lMmdevapi.lib",
            "-lAvrt.lib"
          ]
        }],
        ["OS==\"mac\"", {
          "sources": [
            "src/mac/addon.cpp",
            "src/mac/coreaudio_capture.mm"
          ],
          "xcode_settings": {
            "CLANG_ENABLE_OBJC_ARC": "YES",
            "OTHER_CPLUSPLUSFLAGS": ["-std=c++17", "-ObjC++"],
            "MACOSX_DEPLOYMENT_TARGET": "13.0"
          },
          "link_settings": {
            "libraries": [
              "-framework CoreAudio",
              "-framework CoreMedia",
              "-framework CoreGraphics",
              "-framework ScreenCaptureKit",
              "-framework Foundation"
            ]
          }
        }],
        ["OS==\"linux\"", {
          "sources": [
            "src/linux/addon.cpp",
            "src/linux/pipewire_capture.cpp",
            "src/linux/pulseaudio_capture.cpp",
            "src/linux/jack_capture.cpp",
            "src/linux/alsa_capture.cpp"
          ],
          "cflags_cc": [
            "-std=c++17",
            "<!@(pkg-config --cflags libpipewire-0.3 2>/dev/null || echo '')",
            "<!@(pkg-config --cflags libpulse 2>/dev/null || echo '')",
            "<!@(pkg-config --cflags x11 2>/dev/null || echo '')"
          ],
          "defines": [
            "USE_DYNAMIC_LOADING"
          ],
          "libraries": [
            "-ldl"
          ]
        }]
      ]
    }
  ]
}
