;; /etc/gni.scm
(use-modules (ice-9 ftw)
             (ice-9 match))

(display "[GNI] Director scanning /etc/gni.d/ for modules...\n")

;; Basic Hardware Init
(define (init-hardware)
  (system* "/sbin/udevd" "--daemon")
  (system* "/sbin/udevadm" "trigger" "--action=add")
  (system* "/sbin/udevadm" "settle"))

;; The module loader
(define (run-gni-modules)
  (let ((dir "/etc/gni.d"))
    (if (file-exists? dir)
        (let ((scripts (scandir dir (lambda (f) (string-suffix? ".scm" f)))))
          (if scripts
              (for-each (lambda (f)
                          (display (string-append "  [GNI] Loading: " f "\n"))
                          (load (string-append dir "/" f)))
                        scripts))))))

(init-hardware)
(run-gni-modules)
(display "[GNI] All modules loaded. System is ready.\n")
