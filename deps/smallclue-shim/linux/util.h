/* <util.h> for platforms that do not have one.
 *
 * It is BSD's header -- openpty(), forkpty(), login_tty() -- and glibc has no
 * such file; Linux keeps those in <pty.h>. OpenSSH's packet.c, readconf.c,
 * scp.c and sftp.c include it unconditionally all the same, because configure
 * covers for them: on a host without the header it writes an EMPTY
 * openbsd-compat/include/util.h (see AC_CHECK_HEADERS in configure.ac, the
 * "replacement header files" block), and on a host with one it deletes it.
 *
 * The fork is never configured here -- its config.h is committed, generated on
 * Darwin -- so nothing writes that shim, and a fresh Linux clone failed all
 * four files. The fork deliberately does not commit an empty one either: its
 * openbsd-compat/include is on the Darwin include path too, where an empty
 * util.h would shadow the real header and leave openpty() undeclared.
 *
 * So this is configure's shim, empty just as configure writes it, in the
 * directory that joins the include path only when the host is not Darwin. The
 * shipping build never sees it.
 *
 * Same arrangement as rpc/types.h beside it.
 */
