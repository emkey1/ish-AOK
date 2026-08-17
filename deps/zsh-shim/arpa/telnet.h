/* arpa/telnet.h for a native-zsh build.
 *
 * The iOS SDK omits this header; macOS has it. zsh's zftp module includes it
 * for the Telnet escaping an FTP control connection needs; nothing else in the
 * tree includes it.
 *
 * This carries the WHOLE command-byte block rather than the constants zftp
 * appeared to use. Picking them by grepping the source got IAC/DO/DONT/WILL/
 * WONT and missed IP and SYNCH, which the compiler then found one build later
 * -- and an iOS build is slow enough that guessing twice is worse than copying
 * a table. The values are the wire protocol's (RFC 854), copied from macOS's
 * header, so they cannot drift.
 *
 * As with deps/zsh-shim/termcap.h, this sits ahead of the system directories on
 * BOTH platforms deliberately: one definition set means the macOS build cannot
 * compile against something subtly different from what the device gets.
 */

#ifndef AOK_ZSH_ARPA_TELNET_H
#define AOK_ZSH_ARPA_TELNET_H

#define IAC     255     /* interpret as command */
#define DONT    254     /* you are not to use option */
#define DO      253     /* please, you use option */
#define WONT    252     /* I won't use option */
#define WILL    251     /* I will use option */
#define SB      250     /* interpret as subnegotiation */
#define GA      249     /* you may reverse the line */
#define EL      248     /* erase the current line */
#define EC      247     /* erase the current character */
#define AYT     246     /* are you there */
#define AO      245     /* abort output -- but let prog finish */
#define IP      244     /* interrupt process -- permanently */
#define BREAK   243     /* break */
#define DM      242     /* data mark -- for connection cleaning */
#define NOP     241     /* nop */
#define SE      240     /* end sub negotiation */
#define EOR     239     /* end of record (transparent mode) */
#define ABORT   238     /* abort process */
#define SUSP    237     /* suspend process */
#define xEOF    236     /* end of file: EOF is already used */

#define SYNCH   242     /* for telfunc calls */

#define TELCMD_FIRST    xEOF
#define TELCMD_LAST     IAC
#define TELCMD_OK(x)    ((unsigned int)(x) <= TELCMD_LAST && \
                         (unsigned int)(x) >= TELCMD_FIRST)

#endif /* AOK_ZSH_ARPA_TELNET_H */
