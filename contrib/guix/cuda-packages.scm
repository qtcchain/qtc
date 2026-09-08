(use-modules ((gnu packages base) #:select (tar))
             (gnu packages compression)
             (guix base16)
             (guix build-system trivial)
             (guix download)
             (guix gexp)
             ((guix licenses) #:prefix license:)
             (guix packages))

(define cuda-redist-base-url
  "https://developer.download.nvidia.com/compute/cuda/redist/")

(define (cuda-redist-origin relative-path sha256-hex)
  (origin
    (method url-fetch)
    (uri (string-append cuda-redist-base-url relative-path))
    (hash (content-hash (base16-string->bytevector sha256-hex) sha256))))

(define cuda-12.9.1-cccl
  (cuda-redist-origin
    "cuda_cccl/linux-x86_64/cuda_cccl-linux-x86_64-12.9.27-archive.tar.xz"
    "8b1a5095669e94f2f9afd7715533314d418179e9452be61e2fde4c82a3e542aa"))

(define cuda-12.9.1-cudart
  (cuda-redist-origin
    "cuda_cudart/linux-x86_64/cuda_cudart-linux-x86_64-12.9.79-archive.tar.xz"
    "1f6ad42d4f530b24bfa35894ccf6b7209d2354f59101fd62ec4a6192a184ce99"))

(define cuda-12.9.1-nvcc
  (cuda-redist-origin
    "cuda_nvcc/linux-x86_64/cuda_nvcc-linux-x86_64-12.9.86-archive.tar.xz"
    "7a1a5b652e5ef85c82b721d10672fc9a2dbaab44e9bd3c65a69517bf53998c35"))

(define cuda-12.9.1-cuobjdump
  (cuda-redist-origin
    "cuda_cuobjdump/linux-x86_64/cuda_cuobjdump-linux-x86_64-12.9.82-archive.tar.xz"
    "ee0de40e8c18068bfcc53e73510e7e7a1a80555205347940df67fa525d24452f"))

(define cuda-12.9.1-nvdisasm
  (cuda-redist-origin
    "cuda_nvdisasm/linux-x86_64/cuda_nvdisasm-linux-x86_64-12.9.88-archive.tar.xz"
    "49296dd550e05434185a8588ec639f1325b2de413e2321ddd7e56c5182a476ff"))

(define cuda-12.9.1-nvprune
  (cuda-redist-origin
    "cuda_nvprune/linux-x86_64/cuda_nvprune-linux-x86_64-12.9.82-archive.tar.xz"
    "a06f0e2959a4dd3dbb62a984dbe77b813397022596f5c62d74ddd83b238571f2"))

(define cuda-12.9.1-cuxxfilt
  (cuda-redist-origin
    "cuda_cuxxfilt/linux-x86_64/cuda_cuxxfilt-linux-x86_64-12.9.82-archive.tar.xz"
    "833d7e56351d032717f217212577d369d230e284b2ded4bf151403cc11213add"))

(define cuda-12.9.1-libnvjitlink
  (cuda-redist-origin
    "libnvjitlink/linux-x86_64/libnvjitlink-linux-x86_64-12.9.86-archive.tar.xz"
    "392cac3144b52ba14900bc7259ea6405ae6da88a8c704eab9bbbcc9ba4824b07"))

(define cuda-12.9.1-libnvfatbin
  (cuda-redist-origin
    "libnvfatbin/linux-x86_64/libnvfatbin-linux-x86_64-12.9.82-archive.tar.xz"
    "315be969a303437329bf72d7141babed024fc54f90a10aa748b03be8f826d57b"))

(define cuda-13.2.0-cccl
  (cuda-redist-origin
    "cuda_cccl/linux-x86_64/cuda_cccl-linux-x86_64-13.2.27-archive.tar.xz"
    "56e1bafb29faa87375b0484814870046530b88c0a421909096892f027ec1927b"))

(define cuda-13.2.0-cudart
  (cuda-redist-origin
    "cuda_cudart/linux-x86_64/cuda_cudart-linux-x86_64-13.2.51-archive.tar.xz"
    "539edc1056e44d319f2112e9971c6415d78d4dde04b3f6ffbd20ec808e718526"))

(define cuda-13.2.0-crt
  (cuda-redist-origin
    "cuda_crt/linux-x86_64/cuda_crt-linux-x86_64-13.2.51-archive.tar.xz"
    "fbc31fed55b7255591f3a19f575ca078827f5e6757d317d009f7ec1e69fcde4b"))

(define cuda-13.2.0-culibos
  (cuda-redist-origin
    "cuda_culibos/linux-x86_64/cuda_culibos-linux-x86_64-13.2.51-archive.tar.xz"
    "dece3cdb7954d276e07eb23bee882a24627f431654cdeeef2bcee2d22669d93c"))

(define cuda-13.2.0-nvcc
  (cuda-redist-origin
    "cuda_nvcc/linux-x86_64/cuda_nvcc-linux-x86_64-13.2.51-archive.tar.xz"
    "706b996fefc59dc8d64d317fdf48d0aa84c4ae004eff43009dd918f40c5cc66a"))

(define cuda-13.2.0-libnvvm
  (cuda-redist-origin
    "libnvvm/linux-x86_64/libnvvm-linux-x86_64-13.2.51-archive.tar.xz"
    "e013fce38130d2337ea695aadc5ddd5dcfb78f9107903d72492b9819539749bb"))

(define cuda-13.2.0-libnvptxcompiler
  (cuda-redist-origin
    "libnvptxcompiler/linux-x86_64/libnvptxcompiler-linux-x86_64-13.2.51-archive.tar.xz"
    "52f5aba45e25c3941c38fcae3c4e8c014771c6d9ad73d6427255f9e154bec68d"))

(define cuda-13.2.0-cuobjdump
  (cuda-redist-origin
    "cuda_cuobjdump/linux-x86_64/cuda_cuobjdump-linux-x86_64-13.2.51-archive.tar.xz"
    "f0fb475dfc5b08a0e77cdcd05e7ea15402756ad8418898f4e93ada8b19661271"))

(define cuda-13.2.0-nvdisasm
  (cuda-redist-origin
    "cuda_nvdisasm/linux-x86_64/cuda_nvdisasm-linux-x86_64-13.2.51-archive.tar.xz"
    "0eff70c711a579efa95e141d6e7476a0c041dfb9976a545afca20b555a9fb4a4"))

(define cuda-13.2.0-nvprune
  (cuda-redist-origin
    "cuda_nvprune/linux-x86_64/cuda_nvprune-linux-x86_64-13.2.51-archive.tar.xz"
    "5e7d71f3ee9baa9f0e1b6b72e88fa09e2e63722ed68cc870344f21ddeb28da81"))

(define cuda-13.2.0-cuxxfilt
  (cuda-redist-origin
    "cuda_cuxxfilt/linux-x86_64/cuda_cuxxfilt-linux-x86_64-13.2.51-archive.tar.xz"
    "ddffd73117e125808b9011998668e32d4c2f355f04a9a7c49433ec4a054d836d"))

(define cuda-13.2.0-libnvjitlink
  (cuda-redist-origin
    "libnvjitlink/linux-x86_64/libnvjitlink-linux-x86_64-13.2.51-archive.tar.xz"
    "da61b98d12fcba818967a39fdd282f42718477f02765515223678c0abdd0ce25"))

(define cuda-13.2.0-libnvfatbin
  (cuda-redist-origin
    "libnvfatbin/linux-x86_64/libnvfatbin-linux-x86_64-13.2.51-archive.tar.xz"
    "a390025fe4c3f54f3a4b45313acdc30de183e61e6189f5c56d2571b1d7411203"))

(define (make-cuda-toolkit-qtc name version sdk-version component-origins)
  (package
    (name name)
    (version version)
    (source #f)
    (build-system trivial-build-system)
    (native-inputs (list tar xz))
    (arguments
     (list
      #:modules '((guix build utils)
                  (srfi srfi-1))
      #:builder
      #~(begin
          (use-modules (guix build utils)
                       (srfi srfi-1))
          (let ((out #$output)
                (archives (list #$@component-origins)))
            (mkdir-p out)
            (setenv "PATH"
                    (string-append #$(file-append tar "/bin") ":"
                                   #$(file-append xz "/bin")))
            (for-each
             (lambda (archive)
               (invoke "tar" "--extract" "--file" archive
                       "--xz" "--strip-components=1"
                       "--directory" out))
             archives)
            (when (and (file-exists? (string-append out "/lib"))
                       (not (file-exists? (string-append out "/lib64"))))
              (symlink "lib" (string-append out "/lib64")))
            (call-with-output-file (string-append out "/version.txt")
              (lambda (port)
                (display (string-append "CUDA Version " #$sdk-version "\n") port)))
            (call-with-output-file (string-append out "/version.json")
              (lambda (port)
                (display
                 (string-append
                  "{\n"
                  "   \"cuda\" : {\n"
                  "      \"name\" : \"CUDA SDK\",\n"
                  "      \"version\" : \"" #$sdk-version "\"\n"
                  "   }\n"
                  "}\n")
                 port)))
            (for-each
             (lambda (required)
               (unless (file-exists? (string-append out "/" required))
                 (error "CUDA toolkit component is missing required path"
                        required)))
             '("bin/nvcc"
               "bin/ptxas"
               "include/cuda_runtime.h"
               "lib64/libcudart_static.a"
               "nvvm/libdevice/libdevice.10.bc"))))))
    (home-page "https://developer.nvidia.com/cuda-toolkit")
    (synopsis "Pinned NVIDIA CUDA Toolkit components for QTC Guix release builds")
    (description
     "This package combines pinned NVIDIA CUDA redistrib archives needed to
compile QTC CUDA release binaries.  It includes toolkit/development files only
and does not install an NVIDIA driver.")
    (license (license:non-copyleft "https://docs.nvidia.com/cuda/eula/"))))

(define-public cuda-toolkit-12.9-qtc
  (make-cuda-toolkit-qtc
   "cuda-toolkit-12.9-qtc"
   "12.9.1"
   "12.9.1"
   (list cuda-12.9.1-cccl
         cuda-12.9.1-cudart
         cuda-12.9.1-nvcc
         cuda-12.9.1-cuobjdump
         cuda-12.9.1-nvdisasm
         cuda-12.9.1-nvprune
         cuda-12.9.1-cuxxfilt
         cuda-12.9.1-libnvjitlink
         cuda-12.9.1-libnvfatbin)))

(define-public cuda-toolkit-13-qtc
  (make-cuda-toolkit-qtc
   "cuda-toolkit-13-qtc"
   "13.2.0"
   "13.2.0"
   (list cuda-13.2.0-cccl
         cuda-13.2.0-cudart
         cuda-13.2.0-crt
         cuda-13.2.0-culibos
         cuda-13.2.0-nvcc
         cuda-13.2.0-libnvvm
         cuda-13.2.0-libnvptxcompiler
         cuda-13.2.0-cuobjdump
         cuda-13.2.0-nvdisasm
         cuda-13.2.0-nvprune
         cuda-13.2.0-cuxxfilt
         cuda-13.2.0-libnvjitlink
         cuda-13.2.0-libnvfatbin)))

(list cuda-toolkit-12.9-qtc
      cuda-toolkit-13-qtc)
