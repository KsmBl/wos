This directory holds the kernel: the stripped binary GRUB loads, and the
source it was built from.

Only root may write here. Any other user gets "permission denied" from the
kernel itself, whatever roles they hold -- there is no role that grants
write access to /kernel, by design.

Try it:

    su someuser
    touch /kernel/x        -> permission denied
    cat /kernel/README.txt -> works; reading is unrestricted
