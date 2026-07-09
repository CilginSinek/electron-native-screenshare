{
  "targets": [
    {
      "target_name": "topluyo_capture",
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
            "src/linux/pipewire_capture.cpp"
          ],
          "cflags_cc": [
            "-std=c++17",
            "<!@(pkg-config --cflags libpipewire-0.3 2>/dev/null || echo '')",
            "<!@(pkg-config --cflags x11 2>/dev/null || echo '')"
          ],
          "defines": [
            "<!@(pkg-config --exists libpipewire-0.3 && echo 'HAVE_PIPEWIRE' || echo 'DISABLE_PIPEWIRE')",
            "<!@(pkg-config --exists x11 && echo 'HAVE_X11' || echo 'DISABLE_X11')"
          ]
        }]
      ]
    }
  ]
}
