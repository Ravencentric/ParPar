{
  "variables": {
    "enable_native_tuning%": 1
  },
  "target_defaults": {
    "conditions": [
      ['target_arch=="ia32"', {
        "msvs_settings": {"VCCLCompilerTool": {"EnableEnhancedInstructionSet": "2"}}
      }],
      ['OS!="win" and enable_native_tuning!=0', {
        "variables": {"supports_native%": "<!(<!(echo ${CXX_target:-${CXX:-c++}}) -MM -E src/gyp_warnings.cc -march=native 2>/dev/null || true)"},
        "conditions": [
          ['supports_native!=""', {
            "cflags": ["-march=native"],
            "cxxflags": ["-march=native"],
            "xcode_settings": {
              "OTHER_CFLAGS": ["-march=native"],
              "OTHER_CXXFLAGS": ["-march=native"],
            }
          }, {
            "defines": ["__GYP_WARN_NO_NATIVE"],
          }]
        ]
      }]
    ],
    "cflags": ["-std=gnu99"],
    "msvs_settings": {"VCCLCompilerTool": {"Optimization": "MaxSpeed"}},
    "configurations": {"Release": {
      "cflags": ["-fomit-frame-pointer"],
      "cxxflags": ["-fomit-frame-pointer"],
      "xcode_settings": {
        "OTHER_CFLAGS": ["-fomit-frame-pointer"],
        "OTHER_CXXFLAGS": ["-fomit-frame-pointer"]
      }
    }}
  },
  "targets": [
    {
      "target_name": "parpar_gf",
      "dependencies": ["gf16", "gf16_sse2", "gf16_ssse3", "gf16_avx", "gf16_avx2", "gf16_avx512", "gf16_vbmi", "gf16_gfni", "gf16_gfni_avx2", "gf16_gfni_avx512", "gf16_neon", "gf16_sve", "gf16_sve2", "multi_md5"],
      "sources": ["src/gf.cc", "gf16/module.cc", "gf16/gfmat_coeff.c", "src/gyp_warnings.cc"],
      "include_dirs": ["gf16"],
      "conditions": [
        ['OS=="win"', {
          "msvs_settings": {"VCCLCompilerTool": {"OpenMP": "true"}}
        }, {
          "variables": {
            "supports_omp%": "<!(<!(echo ${CXX_target:-${CXX:-c++}}) -MM -E src/gyp_warnings.cc -fopenmp 2>/dev/null || true)",
            "supports_omp_icc%": "<!(<!(echo ${CXX_target:-${CXX:-c++}}) -MM -E src/gyp_warnings.cc -qopenmp 2>/dev/null || true)",
            "supports_omp_clang%": "<!(<!(echo ${CXX_target:-${CXX:-c++}}) -MM -E src/gyp_warnings.cc -fopenmp=libomp 2>/dev/null || true)"
          },
          "conditions": [
            ['supports_omp_icc!=""', {
              "cflags": ["-qopenmp"],
              "cxxflags": ["-qopenmp"],
              "ldflags": ["-qopenmp"],
              "xcode_settings": {
                "OTHER_CFLAGS": ["-qopenmp"],
                "OTHER_CXXFLAGS": ["-qopenmp"],
                "OTHER_LDFLAGS": ["-qopenmp"]
              }
            }],
            ['supports_omp!="" and supports_omp_clang==""', {
              "cflags": ["-fopenmp"],
              "cxxflags": ["-fopenmp"],
              "ldflags": ["-fopenmp"],
              "xcode_settings": {
                "OTHER_CFLAGS": ["-fopenmp"],
                "OTHER_CXXFLAGS": ["-fopenmp"],
                "OTHER_LDFLAGS": ["-fopenmp"]
              }
            }],
            ['supports_omp_clang!=""', {
              "cflags": ["-fopenmp=libomp"],
              "cxxflags": ["-fopenmp=libomp"],
              "ldflags": ["-fopenmp=libomp"],
              "xcode_settings": {
                "OTHER_CFLAGS": ["-fopenmp=libomp"],
                "OTHER_CXXFLAGS": ["-fopenmp=libomp"],
                "OTHER_LDFLAGS": ["-fopenmp=libomp"]
              }
            }]
          ]
        }]
      ]
    },
    {
      "target_name": "multi_md5",
      "type": "static_library",
      "sources": ["md5/md5.c", "md5/md5-simd.c"],
      "cflags!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"],
      "cxxflags!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"],
      "xcode_settings": {
        "OTHER_CFLAGS!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"],
        "OTHER_CXXFLAGS!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"]
      },
      "msvs_settings": {"VCCLCompilerTool": {"BufferSecurityCheck": "false"}}
    },
    {
      "target_name": "gf16",
      "type": "static_library",
      "defines": ["NDEBUG"],
      "sources": [
        "gf16/gf16mul.cpp",
        "gf16/gf16_lookup.c",
        "gf16/gf_add_generic.c"
      ],
      "cflags": ["-Wno-unused-function"],
      "xcode_settings": {
        "OTHER_CFLAGS": ["-Wno-unused-function"],
        "OTHER_CFLAGS!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"],
        "OTHER_CXXFLAGS!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"],
        "OTHER_LDFLAGS": []
      },
      "cflags!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"],
      "cxxflags!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"],
      "msvs_settings": {"VCCLCompilerTool": {"BufferSecurityCheck": "false"}}
    },
    {
      "target_name": "gf16_sse2",
      "type": "static_library",
      "defines": ["NDEBUG"],
      "sources": [
        "gf16/gf16_xor_sse2.c",
        "gf16/gf16_lookup_sse2.c",
        "gf16/gf_add_sse2.c"
      ],
      "cflags": ["-Wno-unused-function"],
      "xcode_settings": {
        "OTHER_CFLAGS": ["-Wno-unused-function"],
        "OTHER_CFLAGS!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"]
      },
      "cflags!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"],
      "msvs_settings": {"VCCLCompilerTool": {"BufferSecurityCheck": "false"}},
      "conditions": [
        ['target_arch in "ia32 x64"', {
          "cflags": ["-msse2"],
          "cxxflags": ["-msse2"],
          "xcode_settings": {
            "OTHER_CFLAGS": ["-msse2"],
            "OTHER_CXXFLAGS": ["-msse2"],
          }
        }],
        ['OS=="win" and target_arch=="x64"', {
          "sources": ["gf16/xor_jit_stub_masm64.asm"]
        }]
      ]
    },
    {
      "target_name": "gf16_ssse3",
      "type": "static_library",
      "defines": ["NDEBUG"],
      "sources": [
        "gf16/gf16_shuffle_ssse3.c"
      ],
      "cflags": ["-Wno-unused-function"],
      "xcode_settings": {
        "OTHER_CFLAGS": ["-Wno-unused-function"],
        "OTHER_CFLAGS!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"]
      },
      "cflags!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"],
      "msvs_settings": {"VCCLCompilerTool": {"BufferSecurityCheck": "false"}},
      "conditions": [
        ['target_arch in "ia32 x64"', {
          "cflags": ["-mssse3"],
          "cxxflags": ["-mssse3"],
          "xcode_settings": {
            "OTHER_CFLAGS": ["-mssse3"],
            "OTHER_CXXFLAGS": ["-mssse3"],
          }
        }]
      ]
    },
    {
      "target_name": "gf16_avx",
      "type": "static_library",
      "defines": ["NDEBUG"],
      "sources": [
        "gf16/gf16_shuffle_avx.c"
      ],
      "cflags": ["-Wno-unused-function"],
      "xcode_settings": {
        "OTHER_CFLAGS": ["-Wno-unused-function"],
        "OTHER_CFLAGS!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"]
      },
      "cflags!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"],
      "msvs_settings": {"VCCLCompilerTool": {"BufferSecurityCheck": "false"}},
      "conditions": [
        ['target_arch in "ia32 x64"', {
          "msvs_settings": {"VCCLCompilerTool": {"EnableEnhancedInstructionSet": "3"}},
          "cflags": ["-mavx"],
          "cxxflags": ["-mavx"],
          "xcode_settings": {
            "OTHER_CFLAGS": ["-mavx"],
            "OTHER_CXXFLAGS": ["-mavx"],
          }
        }]
      ]
    },
    {
      "target_name": "gf16_avx2",
      "type": "static_library",
      "defines": ["NDEBUG"],
      "sources": [
        "gf16/gf16_xor_avx2.c",
        "gf16/gf16_shuffle_avx2.c",
        "gf16/gf_add_avx2.c"
      ],
      "cflags": ["-Wno-unused-function"],
      "xcode_settings": {
        "OTHER_CFLAGS": ["-Wno-unused-function"],
        "OTHER_CFLAGS!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"]
      },
      "cflags!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"],
      "msvs_settings": {"VCCLCompilerTool": {"BufferSecurityCheck": "false"}},
      "conditions": [
        ['target_arch in "ia32 x64" and OS!="win"', {
          "variables": {"supports_avx2%": "<!(<!(echo ${CC_target:-${CC:-cc}}) -MM -E gf16/gf16_shuffle_avx2.c -mavx2 2>/dev/null || true)"},
          "conditions": [
            ['supports_avx2!=""', {
              "cflags": ["-mavx2"],
              "cxxflags": ["-mavx2"],
              "xcode_settings": {
                "OTHER_CFLAGS": ["-mavx2"],
                "OTHER_CXXFLAGS": ["-mavx2"],
              }
            }]
          ]
        }],
        ['target_arch in "ia32 x64" and OS=="win"', {
          "msvs_settings": {"VCCLCompilerTool": {"EnableEnhancedInstructionSet": "3"}}
        }],
        ['OS=="win" and target_arch=="x64"', {
          "sources": ["gf16/xor_jit_stub_masm64.asm"]
        }]
      ]
    },
    {
      "target_name": "gf16_avx512",
      "type": "static_library",
      "defines": ["NDEBUG"],
      "sources": [
        "gf16/gf16_xor_avx512.c",
        "gf16/gf16_shuffle_avx512.c",
        "gf16/gf_add_avx512.c"
      ],
      "cflags": ["-Wno-unused-function"],
      "xcode_settings": {
        "OTHER_CFLAGS": ["-Wno-unused-function"],
        "OTHER_CFLAGS!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"]
      },
      "cflags!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"],
      "msvs_settings": {"VCCLCompilerTool": {"BufferSecurityCheck": "false"}},
      "conditions": [
        ['target_arch in "ia32 x64" and OS!="win"', {
          "variables": {"supports_avx512%": "<!(<!(echo ${CC_target:-${CC:-cc}}) -MM -E gf16/gf16_shuffle_avx512.c -mavx512vl -mavx512bw 2>/dev/null || true)"},
          "conditions": [
            ['supports_avx512!=""', {
              "cflags": ["-mavx512vl", "-mavx512bw"],
              "cxxflags": ["-mavx512vl", "-mavx512bw"],
              "xcode_settings": {
                "OTHER_CFLAGS": ["-mavx512vl", "-mavx512bw"],
                "OTHER_CXXFLAGS": ["-mavx512vl", "-mavx512bw"],
              }
            }]
          ]
        }],
        ['target_arch in "ia32 x64" and OS=="win"', {
          "msvs_settings": {
            "VCCLCompilerTool": {"AdditionalOptions": ["/arch:AVX512"], "EnableEnhancedInstructionSet": "0"}
          }
        }],
        ['OS=="win" and target_arch=="x64"', {
          "sources": ["gf16/xor_jit_stub_masm64.asm"]
        }]
      ]
    },
    {
      "target_name": "gf16_vbmi",
      "type": "static_library",
      "defines": ["NDEBUG"],
      "sources": [
        "gf16/gf16_shuffle_vbmi.c"
      ],
      "cflags": ["-Wno-unused-function"],
      "xcode_settings": {
        "OTHER_CFLAGS": ["-Wno-unused-function"],
        "OTHER_CFLAGS!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"]
      },
      "cflags!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"],
      "msvs_settings": {"VCCLCompilerTool": {"BufferSecurityCheck": "false"}},
      "conditions": [
        ['target_arch in "ia32 x64" and OS!="win"', {
          "variables": {"supports_vbmi%": "<!(<!(echo ${CC_target:-${CC:-cc}}) -MM -E gf16/gf16_shuffle_vbmi.c -mavx512vl -mavx512vbmi 2>/dev/null || true)"},
          "conditions": [
            ['supports_vbmi!=""', {
              "cflags": ["-mavx512vl", "-mavx512vbmi"],
              "cxxflags": ["-mavx512vl", "-mavx512vbmi"],
              "xcode_settings": {
                "OTHER_CFLAGS": ["-mavx512vl", "-mavx512vbmi"],
                "OTHER_CXXFLAGS": ["-mavx512vl", "-mavx512vbmi"],
              }
            }]
          ]
        }],
        ['target_arch in "ia32 x64" and OS=="win"', {
          "msvs_settings": {
            "VCCLCompilerTool": {"AdditionalOptions": ["/arch:AVX512"], "EnableEnhancedInstructionSet": "0"}
          }
        }]
      ]
    },
    {
      "target_name": "gf16_gfni",
      "type": "static_library",
      "defines": ["NDEBUG"],
      "sources": [
        "gf16/gf16_affine_gfni.c"
      ],
      "cflags": ["-Wno-unused-function"],
      "xcode_settings": {
        "OTHER_CFLAGS": ["-Wno-unused-function"],
        "OTHER_CFLAGS!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"]
      },
      "cflags!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"],
      "msvs_settings": {"VCCLCompilerTool": {"BufferSecurityCheck": "false"}},
      "conditions": [
        ['target_arch in "ia32 x64" and OS!="win"', {
          "variables": {"supports_gfni%": "<!(<!(echo ${CC_target:-${CC:-cc}}) -MM -E gf16/gf16_affine_gfni.c -mgfni -mssse3 2>/dev/null || true)"},
          "conditions": [
            ['supports_gfni!=""', {
              "cflags": ["-mgfni", "-mssse3"],
              "cxxflags": ["-mgfni", "-mssse3"],
              "xcode_settings": {
                "OTHER_CFLAGS": ["-mgfni", "-mssse3"],
                "OTHER_CXXFLAGS": ["-mgfni", "-mssse3"],
              }
            }]
          ]
        }]
      ]
    },
    {
      "target_name": "gf16_gfni_avx2",
      "type": "static_library",
      "defines": ["NDEBUG"],
      "sources": [
        "gf16/gf16_affine_avx2.c"
      ],
      "cflags": ["-Wno-unused-function"],
      "xcode_settings": {
        "OTHER_CFLAGS": ["-Wno-unused-function"],
        "OTHER_CFLAGS!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"]
      },
      "cflags!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"],
      "msvs_settings": {"VCCLCompilerTool": {"BufferSecurityCheck": "false"}},
      "conditions": [
        ['target_arch in "ia32 x64" and OS!="win"', {
          "variables": {"supports_gfni_avx2%": "<!(<!(echo ${CC_target:-${CC:-cc}}) -MM -E gf16/gf16_affine_avx2.c -mgfni -mavx2 2>/dev/null || true)"},
          "conditions": [
            ['supports_gfni_avx2!=""', {
              "cflags": ["-mgfni", "-mavx2"],
              "cxxflags": ["-mgfni", "-mavx2"],
              "xcode_settings": {
                "OTHER_CFLAGS": ["-mgfni", "-mavx2"],
                "OTHER_CXXFLAGS": ["-mgfni", "-mavx2"],
              }
            }]
          ]
        }],
        ['target_arch in "ia32 x64" and OS=="win"', {
          "msvs_settings": {"VCCLCompilerTool": {"EnableEnhancedInstructionSet": "3"}}
        }]
      ]
    },
    {
      "target_name": "gf16_gfni_avx512",
      "type": "static_library",
      "defines": ["NDEBUG"],
      "sources": [
        "gf16/gf16_affine_avx512.c"
      ],
      "cflags": ["-Wno-unused-function"],
      "xcode_settings": {
        "OTHER_CFLAGS": ["-Wno-unused-function"],
        "OTHER_CFLAGS!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"]
      },
      "cflags!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"],
      "msvs_settings": {"VCCLCompilerTool": {"BufferSecurityCheck": "false"}},
      "conditions": [
        ['target_arch in "ia32 x64" and OS!="win"', {
          "variables": {"supports_gfni_avx512%": "<!(<!(echo ${CC_target:-${CC:-cc}}) -MM -E gf16/gf16_affine_avx512.c -mgfni -mavx512vl -mavx512bw 2>/dev/null || true)"},
          "conditions": [
            ['supports_gfni_avx512!=""', {
              "cflags": ["-mgfni", "-mavx512vl", "-mavx512bw"],
              "cxxflags": ["-mgfni", "-mavx512vl", "-mavx512bw"],
              "xcode_settings": {
                "OTHER_CFLAGS": ["-mgfni", "-mavx512vl", "-mavx512bw"],
                "OTHER_CXXFLAGS": ["-mgfni", "-mavx512vl", "-mavx512bw"],
              }
            }]
          ]
        }],
        ['target_arch in "ia32 x64" and OS=="win"', {
          "msvs_settings": {
            "VCCLCompilerTool": {"AdditionalOptions": ["/arch:AVX512"], "EnableEnhancedInstructionSet": "0"}
          }
        }]
      ]
    },
    {
      "target_name": "gf16_neon",
      "type": "static_library",
      "defines": ["NDEBUG"],
      "sources": [
        "gf16/gf16_shuffle_neon.c",
        "gf16/gf16_clmul_neon.c",
        "gf16/gf_add_neon.c"
      ],
      "cflags": ["-Wno-unused-function"],
      "xcode_settings": {
        "OTHER_CFLAGS": ["-Wno-unused-function"],
        "OTHER_CFLAGS!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"]
      },
      "cflags!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"],
      "msvs_settings": {"VCCLCompilerTool": {"BufferSecurityCheck": "false"}},
      "conditions": [
        ['OS!="win" and target_arch=="arm"', {
          "cflags": ["-mfpu=neon"],
          "xcode_settings": {
            "OTHER_CFLAGS": ["-mfpu=neon"]
          }
        }],
        ['OS!="win" and target_arch=="arm" and enable_native_tuning==0', {
          "cflags": ["-march=armv7-a"],
          "xcode_settings": {
            "OTHER_CFLAGS": ["-march=armv7-a"]
          }
        }]
      ]
    },
    {
      "target_name": "gf16_sve",
      "type": "static_library",
      "defines": ["NDEBUG"],
      "sources": [
        "gf16/gf16_shuffle128_sve.c",
        "gf16/gf_add_sve.c"
      ],
      "cflags": ["-Wno-unused-function"],
      "xcode_settings": {
        "OTHER_CFLAGS": ["-Wno-unused-function"],
        "OTHER_CFLAGS!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"]
      },
      "cflags!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"],
      "msvs_settings": {"VCCLCompilerTool": {"BufferSecurityCheck": "false"}},
      "conditions": [
        ['target_arch=="arm64" and OS!="win" and enable_native_tuning==0', {
          "variables": {"supports_sve%": "<!(<!(echo ${CC_target:-${CC:-cc}}) -MM -E gf16/gf16_shuffle128_sve.c -march=armv8-a+sve 2>/dev/null || true)"},
          "conditions": [
            ['supports_sve!=""', {
              "cflags": ["-march=armv8-a+sve"],
              "cxxflags": ["-march=armv8-a+sve"],
              "xcode_settings": {
                "OTHER_CFLAGS": ["-march=armv8-a+sve"],
                "OTHER_CXXFLAGS": ["-march=armv8-a+sve"],
              }
            }]
          ]
        }]
      ]
    },
    {
      "target_name": "gf16_sve2",
      "type": "static_library",
      "defines": ["NDEBUG"],
      "sources": [
        "gf16/gf16_shuffle128_sve2.c",
        "gf16/gf16_shuffle2x128_sve2.c",
        "gf16/gf16_shuffle512_sve2.c",
        "gf16/gf16_clmul_sve2.c",
        "gf16/gf_add_sve2.c"
      ],
      "cflags": ["-Wno-unused-function"],
      "xcode_settings": {
        "OTHER_CFLAGS": ["-Wno-unused-function"],
        "OTHER_CFLAGS!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"]
      },
      "cflags!": ["-fno-omit-frame-pointer", "-fno-tree-vrp", "-fno-strict-aliasing"],
      "msvs_settings": {"VCCLCompilerTool": {"BufferSecurityCheck": "false"}},
      "conditions": [
        ['target_arch=="arm64" and OS!="win" and enable_native_tuning==0', {
          "variables": {"supports_sve2%": "<!(<!(echo ${CC_target:-${CC:-cc}}) -MM -E gf16/gf16_shuffle128_sve2.c -march=armv8-a+sve2 2>/dev/null || true)"},
          "conditions": [
            ['supports_sve2!=""', {
              "cflags": ["-march=armv8-a+sve2"],
              "cxxflags": ["-march=armv8-a+sve2"],
              "xcode_settings": {
                "OTHER_CFLAGS": ["-march=armv8-a+sve2"],
                "OTHER_CXXFLAGS": ["-march=armv8-a+sve2"],
              }
            }]
          ]
        }]
      ]
    }
  ]
}
