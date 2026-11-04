/*
 * umlmm.c -- replacement for Microsoft Bob's UMLMM.DLL (MCI Mail engine)
 *
 * SENDING   : SMTP (plain TCP, AUTH PLAIN / AUTH LOGIN)  -> MMUploadSend
 * RECEIVING : POP3 per user -> IN\\NNNNNNNN.ENV/.MSG     -> MMDownloadMessage
 *
 * Bob's transport (UMLXP.DLL) loads this DLL by name and calls the 23 MM*
 * functions below.  For each outgoing message it writes NNNNNN.ENV (plain
 * text envelope) and NNNNNN.MSG (body) into MAILROOM\OUT and calls:
 *     MMUploadEnvelope -> MMUploadText -> [MMUploadAttachment]* -> MMUploadSend
 * Everything needed is read from those files; the actual SMTP delivery happens
 * in MMUploadSend.
 *
 * Per-user settings live in POP3.INI next to MAILRUN.LOG (see POP3.INI.sample).
 * A trace of the last mail run is written to UMLMM.TRC in the same folder;
 * errors are also appended to MAILRUN.LOG.  Passwords are never logged.
 *
 * No C runtime is used.  WinSock 1.1 (16-bit WINSOCK.DLL) is imported by
 * ordinal through umlmm.lnk.
 */
#include <windows.h>

#ifndef USE_DOS_DATE
#define USE_DOS_DATE 1          /* 0 = skip INT 21h date/time (no Date: header) */
#endif

#define MM_OK       100         /* success                                    */
#define MM_NOMORE   110         /* MMDownloadMessage: nothing (more) to fetch */
#define MM_FAIL     400         /* my own code: delivery failed               */
#define MM_AUTH     401         /* my own code: login rejected                */
#define FAKE_SESSION 0x00420001UL

#define MMAPI int __export __loadds __far __pascal

/* ------------------------------------------------------------------ */
/* WinSock 1.1 (Win16), declared by hand and imported by ordinal       */
/* ------------------------------------------------------------------ */
typedef unsigned int WSOCK;                         /* 16-bit in Win16 */
#define BADSOCK ((WSOCK)0xFFFF)

typedef struct { unsigned short fd_count; WSOCK fd_array[4]; } FDSET4;
typedef struct { long tv_sec; long tv_usec; } TV4;
typedef struct { short sin_family; unsigned short sin_port;
                 unsigned long sin_addr; char sin_zero[8]; } SOCKADDR_IN4;
typedef struct { char FAR *h_name; char FAR * FAR *h_aliases;
                 short h_addrtype; short h_length;
                 char FAR * FAR *h_addr_list; } HOSTENT4;

int      FAR PASCAL ws_startup(WORD ver, void FAR *wsadata);        /* 115 */
int      FAR PASCAL ws_cleanup(void);                               /* 116 */
int      FAR PASCAL ws_geterr(void);                                /* 111 */
WSOCK    FAR PASCAL ws_socket(int af, int type, int proto);         /*  23 */
int      FAR PASCAL ws_connect(WSOCK s, const void FAR *addr, int len); /* 4 */
int      FAR PASCAL ws_send(WSOCK s, const char FAR *buf, int len, int flags); /* 19 */
int      FAR PASCAL ws_recv(WSOCK s, char FAR *buf, int len, int flags);       /* 16 */
int      FAR PASCAL ws_close(WSOCK s);                              /*   3 */
int      FAR PASCAL ws_select(int n, void FAR *r, void FAR *w,
                              void FAR *e, const void FAR *tv);     /*  18 */
HOSTENT4 FAR * FAR PASCAL ws_gethost(const char FAR *name);         /*  52 */

/* ------------------------------------------------------------------ */
/* small string helpers (no C runtime)                                 */
/* ------------------------------------------------------------------ */
static int s_len(const char FAR *s) { int n = 0; while (s[n]) n++; return n; }

static void s_cpy(char FAR *d, const char FAR *s, int max)
{
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static void s_cat(char FAR *d, const char FAR *s, int max)
{
    int n = s_len(d);
    if (n < max - 1) s_cpy(d + n, s, max - n);
}

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int s_icmp(const char FAR *a, const char FAR *b)
{
    while (*a && lc((unsigned char)*a) == lc((unsigned char)*b)) { a++; b++; }
    return lc((unsigned char)*a) - lc((unsigned char)*b);
}

static int s_icontains(const char FAR *hay, const char FAR *needle)
{
    int i, j;
    for (i = 0; hay[i]; i++) {
        for (j = 0; needle[j] && hay[i + j] &&
                    lc((unsigned char)hay[i + j]) == lc((unsigned char)needle[j]); j++)
            ;
        if (needle[j] == 0) return 1;
    }
    return 0;
}

static void fmt_uint(char FAR *out, unsigned v)      /* decimal, NUL-terminated */
{
    char t[8]; int n = 0, i = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) out[i++] = t[--n];
    out[i] = 0;
}

static void put2(char FAR *p, unsigned v)
{
    p[0] = (char)('0' + (v / 10) % 10);
    p[1] = (char)('0' + v % 10);
}

static void hex4(char FAR *p, unsigned v)
{
    static const char H[] = "0123456789abcdef";
    p[0] = H[(v >> 12) & 15]; p[1] = H[(v >> 8) & 15];
    p[2] = H[(v >> 4) & 15];  p[3] = H[v & 15];
}

/* ------------------------------------------------------------------ */
/* date / time (INT 21h)                                               */
/* ------------------------------------------------------------------ */
static void get_dt(unsigned *yr, unsigned *mo, unsigned *dy,
                   unsigned *hh, unsigned *mi, unsigned *ss)
{
#if USE_DOS_DATE
    unsigned cx_, dx_;
    _asm {
        mov ah, 2Ah
        int 21h
        mov cx_, cx
        mov dx_, dx
    }
    *yr = cx_; *mo = dx_ >> 8; *dy = dx_ & 0xFF;
    _asm {
        mov ah, 2Ch
        int 21h
        mov cx_, cx
        mov dx_, dx
    }
    *hh = cx_ >> 8; *mi = cx_ & 0xFF; *ss = dx_ >> 8;
#else
    *yr = 1980; *mo = 1; *dy = 1; *hh = 0; *mi = 0; *ss = 0;
#endif
}

static void stamp(char FAR *out)                 /* "MM-DD-YY HH:MM:SS" */
{
    unsigned yr, mo, dy, hh, mi, ss;
    get_dt(&yr, &mo, &dy, &hh, &mi, &ss);
    put2(out, mo);       out[2] = '-';
    put2(out + 3, dy);   out[5] = '-';
    put2(out + 6, yr % 100); out[8] = ' ';
    put2(out + 9, hh);   out[11] = ':';
    put2(out + 12, mi);  out[14] = ':';
    put2(out + 15, ss);  out[17] = 0;
}

/* ------------------------------------------------------------------ */
/* logging                                                             */
/* ------------------------------------------------------------------ */
static char g_dir[128]    = "C:\\MSBOB\\MAILROOM";    /* mailroom folder     */
static char g_tracef[160] = "C:\\MSBOB\\MAILROOM\\UMLMM.TRC";
static char g_mrlog[160]  = "";                       /* MAILRUN.LOG path    */
static char g_ini[160]    = "C:\\MSBOB\\MAILROOM\\POP3.INI";

static void file_append(const char FAR *path, const char FAR *buf, int n)
{
    int h = _lopen(path, OF_WRITE | OF_SHARE_DENY_NONE);
    if (h == -1) h = _lcreat(path, 0);
    if (h == -1) return;
    _llseek(h, 0L, 2);
    _lwrite(h, (LPCSTR)buf, n);
    _lclose(h);
}

static void tr2(const char FAR *a, const char FAR *b)
{
    char ln[300];
    ln[0] = 0;
    s_cat(ln, a, sizeof(ln) - 3);
    s_cat(ln, b, sizeof(ln) - 3);
    s_cat(ln, "\r\n", sizeof(ln));
    file_append(g_tracef, ln, s_len(ln));
}

static void tr1(const char FAR *a) { tr2(a, ""); }

/* error: trace + MAILRUN.LOG in the engine's own line format */
static void errlog(const char FAR *msg)
{
    char ln[300], st[24];
    tr2("ERROR: ", msg);
    if (g_mrlog[0]) {
        stamp(st);
        ln[0] = 0;
        s_cat(ln, st, sizeof(ln));
        s_cat(ln, " (E) ", sizeof(ln));
        s_cat(ln, msg, sizeof(ln) - 3);
        s_cat(ln, "\r\n", sizeof(ln));
        file_append(g_mrlog, ln, s_len(ln));
    }
}

static void errlog2(const char FAR *a, const char FAR *b)
{
    char t[260];
    t[0] = 0;
    s_cat(t, a, sizeof(t));
    s_cat(t, b, sizeof(t));
    errlog(t);
}

/* ------------------------------------------------------------------ */
/* base64                                                              */
/* ------------------------------------------------------------------ */
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_group(const unsigned char FAR *in, int n, char FAR *out)
{
    unsigned b0 = in[0];
    unsigned b1 = (n > 1) ? in[1] : 0;
    unsigned b2 = (n > 2) ? in[2] : 0;
    out[0] = B64[b0 >> 2];
    out[1] = B64[((b0 & 3) << 4) | (b1 >> 4)];
    out[2] = (n > 1) ? B64[((b1 & 15) << 2) | (b2 >> 6)] : '=';
    out[3] = (n > 2) ? B64[b2 & 63] : '=';
}

static int b64_str(const unsigned char FAR *in, int n, char FAR *out)
{
    int i, o = 0;
    for (i = 0; i < n; i += 3) {
        b64_group(in + i, (n - i > 3) ? 3 : n - i, out + o);
        o += 4;
    }
    out[o] = 0;
    return o;
}

/* ------------------------------------------------------------------ */
/* per-mail-run state                                                  */
/* ------------------------------------------------------------------ */
#define MAXR 24
#define MAXA 6

typedef struct { char kind; char name[64]; char ems[20]; char mbx[100]; } RCPT;

static RCPT g_r[MAXR];
static int  g_nr;
static char g_sname[64], g_sems[20], g_sid[40], g_subj[200];
static char g_aname[MAXA][64], g_apath[MAXA][160];
static int  g_na;
static char g_msgpath[160];
static char g_envbuf[4096];
static int  g_have_env;
static int  g_wsup;
static unsigned g_msgcount;

static void reset_msg(void)
{
    g_nr = 0; g_na = 0; g_have_env = 0;
    g_sname[0] = g_sems[0] = g_sid[0] = g_subj[0] = 0;
    g_msgpath[0] = 0;
}

/* ------------------------------------------------------------------ */
/* envelope (.ENV) parser                                              */
/* ------------------------------------------------------------------ */
static void trim_lead(char FAR * FAR *p) { while (**p == ' ' || **p == '\t') (*p)++; }

static int parse_env(const char FAR *path)
{
    int h;
    UINT got;
    char FAR *p;
    char FAR *line;
    char FAR *colon;
    char FAR *key;
    char FAR *val;
    RCPT FAR *cr = 0;
    int cur = 0;                 /* 1 = sender, 2 = recipient */

    h = _lopen(path, OF_READ | OF_SHARE_DENY_NONE);
    if (h == -1) { errlog2("Cannot open envelope file ", path); return -1; }
    got = _lread(h, g_envbuf, sizeof(g_envbuf) - 1);
    _lclose(h);
    if (got == (UINT)-1) { errlog2("Cannot read envelope file ", path); return -1; }
    g_envbuf[got] = 0;

    p = g_envbuf;
    while (*p) {
        line = p;
        while (*p && *p != '\n') p++;
        if (*p) { *p = 0; p++; }
        { int n = s_len(line); while (n > 0 && line[n - 1] == '\r') line[--n] = 0; }
        if (line[0] == 0) continue;

        colon = line;
        while (*colon && *colon != ':') colon++;
        if (*colon == 0) continue;
        *colon = 0;
        key = line;
        val = colon + 1;
        trim_lead(&val);

        if (s_icmp(key, "From") == 0) {
            s_cpy(g_sname, val, sizeof(g_sname)); cur = 1;
        } else if (s_icmp(key, "TO") == 0 || s_icmp(key, "Cc") == 0 ||
                   s_icmp(key, "Bcc") == 0) {
            if (g_nr < MAXR) {
                cr = &g_r[g_nr++];
                cr->kind = (char)((lc((unsigned char)key[0]) == 't') ? 'T' :
                                  (lc((unsigned char)key[0]) == 'b') ? 'B' : 'C');
                s_cpy(cr->name, val, sizeof(cr->name));
                cr->ems[0] = 0; cr->mbx[0] = 0;
                cur = 2;
            } else cur = 0;
        } else if (s_icmp(key, "EMS") == 0) {
            if (cur == 1) s_cpy(g_sems, val, sizeof(g_sems));
            else if (cur == 2 && cr) s_cpy(cr->ems, val, sizeof(cr->ems));
        } else if (s_icmp(key, "MBX") == 0) {
            if (cur == 1) s_cpy(g_sid, val, sizeof(g_sid));
            else if (cur == 2 && cr) s_cpy(cr->mbx, val, sizeof(cr->mbx));
        } else if (s_icmp(key, "Subject") == 0) {
            s_cpy(g_subj, val, sizeof(g_subj));
        } else if (s_icmp(key, "AttDesc") == 0) {
            if (g_na < MAXA) {
                s_cpy(g_aname[g_na], val, sizeof(g_aname[0]));
                g_apath[g_na][0] = 0;
                g_na++;
            }
            cur = 0;
        } else if (s_icmp(key, "Detach") == 0) {
            if (g_na > 0) s_cpy(g_apath[g_na - 1], val, sizeof(g_apath[0]));
            cur = 0;
        }
        /* all other keys (Date, Handling, Org, ...) are ignored */
    }
    g_have_env = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* network layer                                                       */
/* ------------------------------------------------------------------ */
static WSOCK g_s = BADSOCK;
static char  g_rx[512];
static int   g_rxn, g_rxp;
static char  g_tx[1200];
static int   g_txn;
static char  g_reply[200];
static int   g_cap_plain, g_cap_login, g_cap_tls;

static void net_close(void)
{
    if (g_s != BADSOCK) { ws_close(g_s); g_s = BADSOCK; }
    g_rxn = g_rxp = 0; g_txn = 0;
}

static void set_neterr(const char FAR *what)       /* g_reply = what + WinSock error number */
{
    char num[8];
    fmt_uint(num, (unsigned)ws_geterr());
    s_cpy(g_reply, what, sizeof(g_reply));
    s_cat(g_reply, " (WinSock error ", sizeof(g_reply));
    s_cat(g_reply, num, sizeof(g_reply));
    s_cat(g_reply, ")", sizeof(g_reply));
}

static int net_open(const char FAR *host, unsigned port)
{
    HOSTENT4 FAR *he;
    SOCKADDR_IN4 sa;
    int i;

    he = ws_gethost(host);
    if (he == 0 || he->h_addr_list == 0 || he->h_addr_list[0] == 0) {
        set_neterr("cannot resolve host name");
        return -1;
    }
    sa.sin_family = 2;                                   /* AF_INET */
    sa.sin_port = (unsigned short)(((port & 0xFF) << 8) | (port >> 8));
    sa.sin_addr = *(unsigned long FAR *)he->h_addr_list[0];
    for (i = 0; i < 8; i++) sa.sin_zero[i] = 0;

    g_s = ws_socket(2, 1, 0);                            /* TCP */
    if (g_s == BADSOCK) {
        set_neterr("cannot create socket");
        return -1;
    }
    if (ws_connect(g_s, &sa, sizeof(sa)) != 0) {
        set_neterr("cannot connect to server");
        ws_close(g_s); g_s = BADSOCK;
        return -1;
    }
    g_rxn = g_rxp = 0; g_txn = 0;
    return 0;
}

static int net_flush(void)
{
    int off = 0, n;
    while (off < g_txn) {
        n = ws_send(g_s, g_tx + off, g_txn - off, 0);
        if (n <= 0) { g_txn = 0; return -1; }
        off += n;
    }
    g_txn = 0;
    return 0;
}

static int net_put(const char FAR *p, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (g_txn >= (int)sizeof(g_tx)) { if (net_flush()) return -1; }
        g_tx[g_txn++] = p[i];
    }
    return 0;
}

static int net_puts(const char FAR *s) { return net_put(s, s_len(s)); }

static int net_getc(void)
{
    FDSET4 fds;
    TV4 tv;
    int r;
    if (g_rxp >= g_rxn) {
        fds.fd_count = 1; fds.fd_array[0] = g_s;
        tv.tv_sec = 60L; tv.tv_usec = 0L;
        r = ws_select(0, &fds, 0, 0, &tv);
        if (r <= 0) return -1;
        r = ws_recv(g_s, g_rx, sizeof(g_rx), 0);
        if (r <= 0) return -1;
        g_rxn = r; g_rxp = 0;
    }
    return (unsigned char)g_rx[g_rxp++];
}

static int net_line(char FAR *buf, int max)
{
    int n = 0, c;
    for (;;) {
        c = net_getc();
        if (c < 0) return -1;
        if (c == '\n') break;
        if (c != '\r' && n < max - 1) buf[n++] = (char)c;
    }
    buf[n] = 0;
    return n;
}

/* ------------------------------------------------------------------ */
/* SMTP                                                                */
/* ------------------------------------------------------------------ */
static int smtp_reply(void)         /* numeric reply code, or -1 */
{
    char line[256];
    int n, code;
    for (;;) {
        n = net_line(line, sizeof(line));
        if (n < 0) {
            s_cpy(g_reply, "connection lost or timed out", sizeof(g_reply));
            return -1;
        }
        if (n < 3 || line[0] < '0' || line[0] > '9') {
            s_cpy(g_reply, line, sizeof(g_reply));
            return -1;
        }
        code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
        s_cpy(g_reply, line, sizeof(g_reply));
        tr2("S: ", line);
        if (n > 4) {
            if (s_icontains(line + 4, "AUTH")) {
                if (s_icontains(line + 4, "PLAIN")) g_cap_plain = 1;
                if (s_icontains(line + 4, "LOGIN")) g_cap_login = 1;
            }
            if (s_icontains(line + 4, "STARTTLS")) g_cap_tls = 1;
        }
        if (n == 3 || line[3] != '-') return code;
    }
}

static int smtp_cmd(const char FAR *cmd, const char FAR *shown)
{
    tr2("C: ", shown ? shown : cmd);
    if (net_puts(cmd) || net_puts("\r\n") || net_flush()) {
        s_cpy(g_reply, "send failed", sizeof(g_reply));
        return -1;
    }
    return smtp_reply();
}

static int smtp_auth(const char FAR *user, const char FAR *pass)
{
    unsigned char raw[200];
    char enc[300], cmd[320];
    int n, code, i;

    if (g_cap_plain) {
        n = 0;
        raw[n++] = 0;
        for (i = 0; user[i] && n < 90; i++) raw[n++] = (unsigned char)user[i];
        raw[n++] = 0;
        for (i = 0; pass[i] && n < 190; i++) raw[n++] = (unsigned char)pass[i];
        b64_str(raw, n, enc);
        cmd[0] = 0;
        s_cat(cmd, "AUTH PLAIN ", sizeof(cmd));
        s_cat(cmd, enc, sizeof(cmd));
        code = smtp_cmd(cmd, "AUTH PLAIN ********");
        return (code == 235) ? 0 : -1;
    }
    if (g_cap_login) {
        code = smtp_cmd("AUTH LOGIN", "AUTH LOGIN");
        if (code != 334) return -1;
        b64_str((const unsigned char FAR *)user, s_len(user), enc);
        code = smtp_cmd(enc, "<user name>");
        if (code != 334) return -1;
        b64_str((const unsigned char FAR *)pass, s_len(pass), enc);
        code = smtp_cmd(enc, "<password>");
        return (code == 235) ? 0 : -1;
    }
    s_cpy(g_reply, "server offers no AUTH PLAIN or AUTH LOGIN", sizeof(g_reply));
    return -1;
}

/* ------------------------------------------------------------------ */
/* message construction                                                */
/* ------------------------------------------------------------------ */
static char g_host[100], g_hello[80], g_email[100];
static char g_user[80], g_pass[80];
static unsigned g_port;
static char g_bound[48];
static BYTE g_io[600];

static void clean(char FAR *dst, const char FAR *src, int max)   /* header-safe */
{
    int i = 0;
    while (*src && i < max - 1) {
        unsigned char c = (unsigned char)*src++;
        if (c < 32 || c > 126 || c == '"' || c == '\\') c = (c == '"' || c == '\\') ? ' ' : '?';
        dst[i++] = (char)c;
    }
    dst[i] = 0;
}

static void cfg_override(const char FAR *sect, const char FAR *key, char FAR *dst, int max)
{
    char t[100];
    GetPrivateProfileString(sect, key, "", t, sizeof(t), g_ini);
    if (t[0]) s_cpy(dst, t, max);
}

static int load_cfg(void)
{
    char sect[80], sect2[80], tmp[100];

    GetPrivateProfileString("SERVER", "SmtpHost", "msbob.org", g_host, sizeof(g_host), g_ini);
    g_port = GetPrivateProfileInt("SERVER", "SmtpPort", 25, g_ini);
    GetPrivateProfileString("SERVER", "Hello", "localhost", g_hello, sizeof(g_hello), g_ini);

    sect[0] = 0; s_cat(sect, "USER.", sizeof(sect)); s_cat(sect, g_sid, sizeof(sect));
    GetPrivateProfileString(sect, "Email", "", g_email, sizeof(g_email), g_ini);
    if (!g_email[0]) {
        sect2[0] = 0; s_cat(sect2, "USER.", sizeof(sect2)); s_cat(sect2, g_sname, sizeof(sect2));
        GetPrivateProfileString(sect2, "Email", "", g_email, sizeof(g_email), g_ini);
        if (g_email[0]) s_cpy(sect, sect2, sizeof(sect));
    }
    if (!g_email[0]) {
        tmp[0] = 0;
        s_cat(tmp, "no [", sizeof(tmp)); s_cat(tmp, sect, sizeof(tmp));
        s_cat(tmp, "] section with Email= in ", sizeof(tmp));
        errlog2(tmp, g_ini);
        return -1;
    }
    GetPrivateProfileString(sect, "User", "", g_user, sizeof(g_user), g_ini);
    GetPrivateProfileString(sect, "Password", "", g_pass, sizeof(g_pass), g_ini);
    cfg_override(sect, "SmtpUser", g_user, sizeof(g_user));
    cfg_override(sect, "SmtpPassword", g_pass, sizeof(g_pass));
    return 0;
}

static int hdr(const char FAR *name, const char FAR *val)
{
    return net_puts(name) || net_puts(val) || net_puts("\r\n");
}

static int send_addr_list(const char FAR *label, char kind)
{
    int i, first = 1;
    char nm[70], item[220];
    for (i = 0; i < g_nr; i++) {
        if (g_r[i].kind != kind || s_icmp(g_r[i].ems, "INTERNET") != 0 || !g_r[i].mbx[0])
            continue;
        clean(nm, g_r[i].name, sizeof(nm));
        item[0] = 0;
        if (nm[0]) { s_cat(item, "\"", sizeof(item)); s_cat(item, nm, sizeof(item)); s_cat(item, "\" ", sizeof(item)); }
        s_cat(item, "<", sizeof(item)); s_cat(item, g_r[i].mbx, sizeof(item)); s_cat(item, ">", sizeof(item));
        if (first) { if (net_puts(label)) return -1; first = 0; }
        else if (net_puts(",\r\n\t")) return -1;
        if (net_puts(item)) return -1;
    }
    if (!first && net_puts("\r\n")) return -1;
    return 0;
}

static int send_body(const char FAR *path)      /* text file -> dot-stuffed CRLF text */
{
    int h, bol = 1, pend_cr = 0;
    UINT got, i;
    unsigned c;

    h = _lopen(path, OF_READ | OF_SHARE_DENY_NONE);
    if (h == -1) { errlog2("Cannot open message text file ", path); return -1; }
    for (;;) {
        got = _lread(h, g_io, sizeof(g_io));
        if (got == (UINT)-1) { _lclose(h); errlog2("Read error on ", path); return -1; }
        if (got == 0) break;
        for (i = 0; i < got; i++) {
            c = g_io[i];
            if (pend_cr) {
                if (net_puts("\r\n")) { _lclose(h); return -1; }
                bol = 1; pend_cr = 0;
                if (c == '\n') continue;
            }
            if (c == '\r') { pend_cr = 1; continue; }
            if (c == '\n') { if (net_puts("\r\n")) { _lclose(h); return -1; } bol = 1; continue; }
            if (c == 0) continue;
            if (bol && c == '.') { if (net_put(".", 1)) { _lclose(h); return -1; } }
            { char ch = (char)c; if (net_put(&ch, 1)) { _lclose(h); return -1; } }
            bol = 0;
        }
    }
    _lclose(h);
    if (pend_cr) { if (net_puts("\r\n")) return -1; bol = 1; }
    if (!bol) { if (net_puts("\r\n")) return -1; }
    return 0;
}

static int send_attachment(const char FAR *path)    /* file -> base64 lines */
{
    int h;
    UINT got, k;
    char out[8];

    h = _lopen(path, OF_READ | OF_SHARE_DENY_NONE);
    if (h == -1) { errlog2("Cannot open attachment file ", path); return -1; }
    for (;;) {
        got = _lread(h, g_io, 57);
        if (got == (UINT)-1) { _lclose(h); errlog2("Read error on ", path); return -1; }
        if (got == 0) break;
        for (k = 0; k < got; k += 3) {
            b64_group(g_io + k, (got - k > 3) ? 3 : (int)(got - k), out);
            if (net_put(out, 4)) { _lclose(h); return -1; }
        }
        if (net_puts("\r\n")) { _lclose(h); return -1; }
        if (got < 57) break;
    }
    _lclose(h);
    return 0;
}

static void make_boundary(void)
{
    union { DWORD l; WORD w[2]; } u;
    u.l = GetTickCount();
    g_msgcount++;
    g_bound[0] = 0;
    s_cat(g_bound, "=_BobGate_", sizeof(g_bound));
    hex4(g_bound + s_len(g_bound), u.w[1]); g_bound[s_len(g_bound) + 4] = 0;
    hex4(g_bound + s_len(g_bound), u.w[0]); g_bound[s_len(g_bound) + 4] = 0;
    hex4(g_bound + s_len(g_bound), g_msgcount); g_bound[s_len(g_bound) + 4] = 0;
}

static int send_headers(void)
{
    static const char *MON[] = { "Jan","Feb","Mar","Apr","May","Jun",
                                 "Jul","Aug","Sep","Oct","Nov","Dec" };
    char nm[70], line[260], num[8], datebuf[48], mid[160];
    const char *at;
    unsigned yr, mo, dy, hh, mi, ss;
    int i;

    /* From */
    clean(nm, g_sname, sizeof(nm));
    line[0] = 0;
    if (nm[0]) { s_cat(line, "\"", sizeof(line)); s_cat(line, nm, sizeof(line)); s_cat(line, "\" ", sizeof(line)); }
    s_cat(line, "<", sizeof(line)); s_cat(line, g_email, sizeof(line)); s_cat(line, ">", sizeof(line));
    if (hdr("From: ", line)) return -1;

    if (send_addr_list("To: ", 'T')) return -1;
    if (send_addr_list("Cc: ", 'C')) return -1;

    clean(line, g_subj, sizeof(line));
    if (hdr("Subject: ", line)) return -1;

    /* Date */
#if USE_DOS_DATE
    get_dt(&yr, &mo, &dy, &hh, &mi, &ss);
    if (mo < 1 || mo > 12) mo = 1;
    datebuf[0] = 0;
    fmt_uint(num, dy); s_cat(datebuf, num, sizeof(datebuf)); s_cat(datebuf, " ", sizeof(datebuf));
    s_cat(datebuf, MON[mo - 1], sizeof(datebuf)); s_cat(datebuf, " ", sizeof(datebuf));
    fmt_uint(num, yr); s_cat(datebuf, num, sizeof(datebuf)); s_cat(datebuf, " ", sizeof(datebuf));
    { char t[4]; put2(t, hh); t[2] = ':'; t[3] = 0; s_cat(datebuf, t, sizeof(datebuf));
      put2(t, mi); t[2] = ':'; s_cat(datebuf, t, sizeof(datebuf));
      put2(t, ss); t[2] = 0; s_cat(datebuf, t, sizeof(datebuf)); }
    s_cat(datebuf, " -0000", sizeof(datebuf));
    if (hdr("Date: ", datebuf)) return -1;
#endif

    /* Message-ID */
    make_boundary();
    at = 0;
    for (i = 0; g_email[i]; i++) if (g_email[i] == '@') at = &g_email[i + 1];
    mid[0] = 0;
    s_cat(mid, "<", sizeof(mid));
    s_cat(mid, g_bound + 10, sizeof(mid));
    s_cat(mid, "@", sizeof(mid));
    s_cat(mid, at ? at : "bob.local", sizeof(mid));
    s_cat(mid, ">", sizeof(mid));
    if (hdr("Message-ID: ", mid)) return -1;

    if (hdr("MIME-Version: ", "1.0")) return -1;
    if (hdr("X-Mailer: ", "Microsoft Bob / UMLMM SMTP gateway")) return -1;
    return 0;
}

static int smtp_deliver(void)       /* 0 = delivered, else MM_xxx */
{
    int code, i, accepted = 0, rc = MM_FAIL, nattach = 0;
    char cmd[200], t[200];

    if (!g_have_env) { errlog("MMUploadSend called without an envelope"); return MM_FAIL; }
    if (!g_msgpath[0]) { errlog("MMUploadSend called without a message text file"); return MM_FAIL; }
    if (load_cfg()) return MM_FAIL;

    for (i = 0; i < g_na; i++) if (g_apath[i][0]) nattach++;

    tr2("Sender id=", g_sid);
    tr2("Sender email=", g_email);
    tr2("SMTP host=", g_host);

    if (net_open(g_host, g_port)) { errlog2("SMTP: ", g_reply); return MM_FAIL; }

    code = smtp_reply();
    if (code != 220) { errlog2("SMTP greeting: ", g_reply); goto fail; }

    g_cap_plain = g_cap_login = g_cap_tls = 0;
    cmd[0] = 0; s_cat(cmd, "EHLO ", sizeof(cmd)); s_cat(cmd, g_hello, sizeof(cmd));
    code = smtp_cmd(cmd, 0);
    if (code != 250) {
        cmd[0] = 0; s_cat(cmd, "HELO ", sizeof(cmd)); s_cat(cmd, g_hello, sizeof(cmd));
        code = smtp_cmd(cmd, 0);
        if (code != 250) { errlog2("SMTP HELO rejected: ", g_reply); goto fail; }
    }

    if (g_user[0]) {
        if (smtp_auth(g_user, g_pass)) {
            errlog2("SMTP login failed: ", g_reply);
            if (g_cap_tls) errlog("Server also offers STARTTLS; this gateway cannot use TLS");
            rc = MM_AUTH;
            goto fail;
        }
    } else {
        tr1("No User= for this mailbox, sending without AUTH");
    }

    cmd[0] = 0; s_cat(cmd, "MAIL FROM:<", sizeof(cmd)); s_cat(cmd, g_email, sizeof(cmd)); s_cat(cmd, ">", sizeof(cmd));
    code = smtp_cmd(cmd, 0);
    if (code != 250) { errlog2("SMTP sender rejected: ", g_reply); goto fail; }

    for (i = 0; i < g_nr; i++) {
        if (s_icmp(g_r[i].ems, "INTERNET") != 0 || !g_r[i].mbx[0]) {
            t[0] = 0;
            s_cat(t, "Recipient ", sizeof(t)); s_cat(t, g_r[i].name, sizeof(t));
            s_cat(t, " (", sizeof(t)); s_cat(t, g_r[i].ems, sizeof(t));
            s_cat(t, ") skipped: only INTERNET addresses can be delivered", sizeof(t));
            errlog(t);
            continue;
        }
        cmd[0] = 0; s_cat(cmd, "RCPT TO:<", sizeof(cmd)); s_cat(cmd, g_r[i].mbx, sizeof(cmd)); s_cat(cmd, ">", sizeof(cmd));
        code = smtp_cmd(cmd, 0);
        if (code == 250 || code == 251) accepted++;
        else {
            t[0] = 0; s_cat(t, "Recipient ", sizeof(t)); s_cat(t, g_r[i].mbx, sizeof(t));
            s_cat(t, " refused: ", sizeof(t)); s_cat(t, g_reply, sizeof(t));
            errlog(t);
        }
    }
    if (accepted == 0) { errlog("No recipient was accepted by the server"); goto fail; }

    code = smtp_cmd("DATA", 0);
    if (code != 354) { errlog2("SMTP DATA refused: ", g_reply); goto fail; }
    tr1("C: <message data>");

    if (send_headers()) goto sendfail;

    if (nattach) {
        if (net_puts("Content-Type: multipart/mixed; boundary=\"") ||
            net_puts(g_bound) || net_puts("\"\r\n\r\n")) goto sendfail;
        if (net_puts("--") || net_puts(g_bound) || net_puts("\r\n")) goto sendfail;
    }
    if (hdr("Content-Type: ", "text/plain; charset=windows-1252")) goto sendfail;
    if (hdr("Content-Transfer-Encoding: ", "8bit")) goto sendfail;
    if (net_puts("\r\n")) goto sendfail;
    if (send_body(g_msgpath)) goto sendfail;

    for (i = 0; i < g_na; i++) {
        char fn[70];
        if (!g_apath[i][0]) continue;
        clean(fn, g_aname[i], sizeof(fn));
        net_puts("--"); net_puts(g_bound); net_puts("\r\n");
        net_puts("Content-Type: application/octet-stream; name=\""); net_puts(fn); net_puts("\"\r\n");
        net_puts("Content-Disposition: attachment; filename=\""); net_puts(fn); net_puts("\"\r\n");
        net_puts("Content-Transfer-Encoding: base64\r\n\r\n");
        if (send_attachment(g_apath[i])) goto sendfail;
    }
    if (nattach) {
        net_puts("--"); net_puts(g_bound); net_puts("--\r\n");
    }
    if (net_puts(".\r\n") || net_flush()) goto sendfail;

    code = smtp_reply();
    if (code != 250) { errlog2("SMTP message refused: ", g_reply); goto fail; }

    smtp_cmd("QUIT", 0);
    net_close();
    tr1("Message delivered");
    return 0;

sendfail:
    errlog("Network error while sending the message");
fail:
    net_close();
    return rc;
}

/* ================================================================== */
/* RECEIVING: POP3  ->  IN\NNNNNNNN.ENV / .MSG / .000 ...              */
/* ================================================================== */
#define MAXATT 8
#define M_TEXT   0
#define M_HTML   1
#define M_ATTACH 2
#define M_SKIP   3

typedef struct {
    char type[40];
    char boundary[72];
    char charset[24];
    char cte[24];
    char fname[72];
    char disp[16];
} ENTH;

static ENTH g_enth[6];
static char g_bnd[4][72];
static int  g_nb, g_end_level, g_end_close;
static char g_line[1100];
static char g_hn[40], g_hv[1000];
static char g_tmpa[220], g_tmpb[220];
static int  g_rf = -1;
static char g_rdb[512];
static int  g_rdn, g_rdp;

static char g_in_from[200], g_in_subj[200], g_in_date[64], g_in_to[400], g_in_cc[400];
static int  g_have_body, g_natt;
static char g_attn[MAXATT][72], g_attp[MAXATT][170];
static char g_indir[170], g_cfgdir[170], g_bufbase[12], g_base[12], g_msgout[180];

static char g_phost[100], g_pop_users[160], g_datestyle[8], g_cfgin[170];
static unsigned g_pport;
static int  g_delete = 1, g_keepraw = 1, g_showrcpt = 1;
static int  g_maxrun = 25, g_newest, g_stored, g_pop_step = 1;
static int  g_dl_state;                  /* 0 not started, 1 running, 2 finished */
static int  g_pop_open, g_pop_n, g_pop_i, g_upos;
static char g_pop_id[40], g_pop_name[64], g_pop_user[80], g_pop_pass[80], g_pop_email[100];
static unsigned g_fcount;
static int  g_pop_dead;                  /* connection to the POP3 server failed */

/* ---------- buffered output file ---------- */
static int  g_of = -1;
static char g_ob[512];
static int  g_obn;

static void out_flush(void)
{
    if (g_of != -1 && g_obn > 0) _lwrite(g_of, (LPCSTR)g_ob, g_obn);
    g_obn = 0;
}

static void out_close(void)
{
    if (g_of != -1) { out_flush(); _lclose(g_of); g_of = -1; }
    g_obn = 0;
}

static int out_open(const char FAR *path)
{
    out_close();
    g_of = _lcreat(path, 0);
    g_obn = 0;
    return (g_of == -1) ? -1 : 0;
}

static void out_put(unsigned c)
{
    if (g_of == -1) return;
    if (g_obn >= (int)sizeof(g_ob)) out_flush();
    g_ob[g_obn++] = (char)c;
}

static void out_puts(const char FAR *s) { while (*s) out_put((unsigned char)*s++); }

static void del_file(const char FAR *path)
{
    OFSTRUCT of;
    OpenFile(path, &of, OF_DELETE);
}

static int file_exists(const char FAR *path)
{
    int h = _lopen(path, OF_READ | OF_SHARE_DENY_NONE);
    if (h == -1) return 0;
    _lclose(h);
    return 1;
}

/* ---------- buffered input file (line reader) ---------- */
static int rd_getc(void)
{
    UINT n;
    if (g_rdp >= g_rdn) {
        n = _lread(g_rf, g_rdb, sizeof(g_rdb));
        if (n == (UINT)-1 || n == 0) return -1;
        g_rdn = (int)n; g_rdp = 0;
    }
    return (unsigned char)g_rdb[g_rdp++];
}

static int rd_line(char FAR *buf, int max)       /* -1 at end of file */
{
    int n = 0, c;
    c = rd_getc();
    if (c < 0) return -1;
    while (c >= 0 && c != '\n') {
        if (c != '\r' && n < max - 1) buf[n++] = (char)c;
        c = rd_getc();
    }
    buf[n] = 0;
    return n;
}

/* ---------- character handling ---------- */
static int starts_ci(const char FAR *s, const char FAR *p)
{
    while (*p) {
        if (lc((unsigned char)*s) != lc((unsigned char)*p)) return 0;
        s++; p++;
    }
    return 1;
}

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int b64_val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static unsigned cp_to_1252(unsigned cp)
{
    if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) return cp;
    switch (cp) {
    case 0x20AC: return 0x80;  case 0x201A: return 0x82;  case 0x0192: return 0x83;
    case 0x201E: return 0x84;  case 0x2026: return 0x85;  case 0x2020: return 0x86;
    case 0x2021: return 0x87;  case 0x02C6: return 0x88;  case 0x2030: return 0x89;
    case 0x0160: return 0x8A;  case 0x2039: return 0x8B;  case 0x0152: return 0x8C;
    case 0x017D: return 0x8E;  case 0x2018: return 0x91;  case 0x2019: return 0x92;
    case 0x201C: return 0x93;  case 0x201D: return 0x94;  case 0x2022: return 0x95;
    case 0x2013: return 0x96;  case 0x2014: return 0x97;  case 0x02DC: return 0x98;
    case 0x2122: return 0x99;  case 0x0161: return 0x9A;  case 0x203A: return 0x9B;
    case 0x0153: return 0x9C;  case 0x017E: return 0x9E;  case 0x0178: return 0x9F;
    }
    return '?';
}

/* final character sink: message text, or the MIME-word buffer */
static int  g_sink;                       /* 0 = text pipeline, 1 = word buffer */
static char FAR *g_mwp;
static int  g_mwo, g_mwmax;

static void final_char(unsigned ch)
{
    if (g_sink == 1) {
        if (g_mwo < g_mwmax - 1) g_mwp[g_mwo++] = (char)ch;
        return;
    }
    if (ch == '\r') return;
    if (ch == '\n') { out_put('\r'); out_put('\n'); }
    else out_put(ch);
}

static int      g_cs_utf8, g_u8need, g_u8big;
static unsigned g_u8cp;

static void u8_in(unsigned c)
{
    if (!g_cs_utf8 || c < 0x80) { final_char(c); return; }
    if (g_u8need) {
        if ((c & 0xC0) == 0x80) {
            if (!g_u8big) g_u8cp = (g_u8cp << 6) | (c & 0x3F);
            if (--g_u8need == 0) final_char(g_u8big ? '?' : cp_to_1252(g_u8cp));
            return;
        }
        g_u8need = 0;
        final_char('?');
    }
    if (c >= 0xF0)      { g_u8need = 3; g_u8big = 1; g_u8cp = 0; }
    else if (c >= 0xE0) { g_u8need = 2; g_u8big = 0; g_u8cp = c & 15; }
    else if (c >= 0xC0) { g_u8need = 1; g_u8big = 0; g_u8cp = c & 31; }
    else final_char('?');
}

/* ---------- HTML -> text ---------- */
static int  g_h_tag, g_h_skip, g_h_ent, g_h_closing, g_h_nl, g_h_sp;
static char g_h_name[12], g_h_ebuf[10];
static int  g_h_nlen, g_h_elen, g_h_namedone;

static void html_reset(void)
{
    g_h_tag = g_h_skip = g_h_ent = g_h_closing = 0;
    g_h_nl = 2; g_h_sp = 1; g_h_nlen = g_h_elen = g_h_namedone = 0;
}

static void h_newline(int n)
{
    while (g_h_nl < n) { final_char('\n'); g_h_nl++; }
    g_h_sp = 1;
}

static void h_char(unsigned c)           /* a visible character (byte stream) */
{
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        if (!g_h_sp) { final_char(' '); g_h_sp = 1; }
        return;
    }
    u8_in(c);
    g_h_nl = 0; g_h_sp = 0;
}

static void h_entity(void)
{
    unsigned cp = 0;
    int i;
    g_h_ebuf[g_h_elen] = 0;
    if (s_icmp(g_h_ebuf, "amp") == 0) cp = '&';
    else if (s_icmp(g_h_ebuf, "lt") == 0) cp = '<';
    else if (s_icmp(g_h_ebuf, "gt") == 0) cp = '>';
    else if (s_icmp(g_h_ebuf, "quot") == 0) cp = '"';
    else if (s_icmp(g_h_ebuf, "apos") == 0) cp = '\'';
    else if (s_icmp(g_h_ebuf, "nbsp") == 0) cp = ' ';
    else if (g_h_ebuf[0] == '#') {
        if (g_h_ebuf[1] == 'x' || g_h_ebuf[1] == 'X') {
            for (i = 2; g_h_ebuf[i]; i++) { if (hexv(g_h_ebuf[i]) < 0) { cp = 0; break; } if (cp > 0x0FFF) { cp = 0xFFFF; break; } cp = cp * 16 + hexv(g_h_ebuf[i]); }
        } else {
            for (i = 1; g_h_ebuf[i]; i++) { if (g_h_ebuf[i] < '0' || g_h_ebuf[i] > '9') { cp = 0; break; } if (cp > 6500) { cp = 0xFFFF; break; } cp = cp * 10 + (g_h_ebuf[i] - '0'); }
        }
        if (cp) cp = cp_to_1252(cp);
    }
    if (cp == ' ') { h_char(' '); return; }
    if (cp) { final_char(cp); g_h_nl = 0; g_h_sp = 0; }
    else { final_char('&'); for (i = 0; g_h_ebuf[i]; i++) final_char((unsigned char)g_h_ebuf[i]); final_char(';'); g_h_nl = 0; g_h_sp = 0; }
}

static void html_in(unsigned c)
{
    if (g_h_tag) {
        if (c == '>') {
            g_h_name[g_h_nlen] = 0;
            if (g_h_closing) {
                if (s_icmp(g_h_name, "style") == 0 || s_icmp(g_h_name, "script") == 0) g_h_skip = 0;
            } else if (s_icmp(g_h_name, "style") == 0 || s_icmp(g_h_name, "script") == 0) {
                g_h_skip = 1;
            }
            if (!g_h_skip) {
                if (s_icmp(g_h_name, "br") == 0 || s_icmp(g_h_name, "li") == 0) h_newline(1);
                else if (s_icmp(g_h_name, "p") == 0 || s_icmp(g_h_name, "div") == 0 ||
                         s_icmp(g_h_name, "tr") == 0 || s_icmp(g_h_name, "table") == 0 ||
                         s_icmp(g_h_name, "h1") == 0 || s_icmp(g_h_name, "h2") == 0 ||
                         s_icmp(g_h_name, "h3") == 0 || s_icmp(g_h_name, "blockquote") == 0)
                    h_newline(g_h_closing ? 2 : 1);
            }
            g_h_tag = 0;
        } else if (!g_h_namedone) {
            if (c == '/' && g_h_nlen == 0) g_h_closing = 1;
            else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') g_h_namedone = 1;
            else if (g_h_nlen < (int)sizeof(g_h_name) - 1) g_h_name[g_h_nlen++] = (char)c;
        }
        return;
    }
    if (c == '<') {
        g_h_tag = 1; g_h_closing = 0; g_h_nlen = 0; g_h_namedone = 0; g_h_name[0] = 0;
        if (g_h_ent) { g_h_ent = 0; h_char('&'); }
        return;
    }
    if (g_h_skip) return;
    if (g_h_ent) {
        if (c == ';') { g_h_ent = 0; h_entity(); return; }
        if (g_h_elen < (int)sizeof(g_h_ebuf) - 2 && c != ' ' && c != '&') { g_h_ebuf[g_h_elen++] = (char)c; return; }
        { int i; g_h_ent = 0; h_char('&'); for (i = 0; i < g_h_elen; i++) h_char((unsigned char)g_h_ebuf[i]); }
    }
    if (c == '&') { g_h_ent = 1; g_h_elen = 0; return; }
    h_char(c);
}

/* ---------- body decoding: base64 / quoted-printable / raw ---------- */
static int g_mode;
static int g_bv[4], g_bn;

static void emit_byte(unsigned c)
{
    switch (g_mode) {
    case M_TEXT:   u8_in(c); break;
    case M_HTML:   html_in(c); break;
    case M_ATTACH: out_put(c); break;
    default: break;
    }
}

static void emit_nl(void)
{
    if (g_mode == M_ATTACH) { out_put('\r'); out_put('\n'); }
    else if (g_mode == M_TEXT || g_mode == M_HTML) emit_byte('\n');
}

static void b64_char(int c)
{
    int v;
    if (c == '=') {
        if (g_bn >= 2) {
            emit_byte(((g_bv[0] << 2) | (g_bv[1] >> 4)) & 0xFF);
            if (g_bn == 3) emit_byte(((g_bv[1] << 4) | (g_bv[2] >> 2)) & 0xFF);
        }
        g_bn = 0;
        return;
    }
    v = b64_val(c);
    if (v < 0) return;
    g_bv[g_bn++] = v;
    if (g_bn == 4) {
        emit_byte(((g_bv[0] << 2) | (g_bv[1] >> 4)) & 0xFF);
        emit_byte(((g_bv[1] << 4) | (g_bv[2] >> 2)) & 0xFF);
        emit_byte(((g_bv[2] << 6) | g_bv[3]) & 0xFF);
        g_bn = 0;
    }
}

static void decode_line(const char FAR *ln, int n, int cte)   /* 0 raw, 1 b64, 2 qp */
{
    int i, soft = 0, a, b;
    if (cte == 1) { for (i = 0; i < n; i++) b64_char((unsigned char)ln[i]); return; }
    if (cte == 2) {
        for (i = 0; i < n; i++) {
            if (ln[i] == '=') {
                if (i + 1 >= n) { soft = 1; break; }
                a = hexv(ln[i + 1]);
                b = (i + 2 < n) ? hexv(ln[i + 2]) : -1;
                if (a >= 0 && b >= 0) { emit_byte((unsigned)((a << 4) | b)); i += 2; continue; }
                emit_byte('=');
                continue;
            }
            emit_byte((unsigned char)ln[i]);
        }
        if (!soft) emit_nl();
        return;
    }
    for (i = 0; i < n; i++) emit_byte((unsigned char)ln[i]);
    emit_nl();
}

/* ---------- RFC 2047 encoded words + helpers ---------- */
static void mime_words(const char FAR *in, char FAR *out, int max)
{
    int i = 0, j, k, start, end, o, csn, enc, utf8, prevenc = 0, v[4], nv;
    char cs[24];

    g_mwp = out; g_mwo = 0; g_mwmax = max; g_sink = 1;
    while (in[i] && g_mwo < max - 1) {
        if (in[i] == '=' && in[i + 1] == '?') {
            j = i + 2; csn = 0;
            while (in[j] && in[j] != '?' && csn < 23) cs[csn++] = in[j++];
            cs[csn] = 0;
            if (in[j] == '?' && in[j + 1] && in[j + 2] == '?') {
                enc = lc((unsigned char)in[j + 1]);
                start = j + 3; end = start;
                while (in[end] && !(in[end] == '?' && in[end + 1] == '=')) end++;
                if (in[end] == '?') {
                    utf8 = s_icontains(cs, "utf-8") || s_icontains(cs, "utf8");
                    g_cs_utf8 = utf8; g_u8need = 0;
                    if (enc == 'b') {
                        nv = 0;
                        for (k = start; k < end; k++) {
                            if (in[k] == '=') break;
                            o = b64_val((unsigned char)in[k]);
                            if (o < 0) continue;
                            v[nv++] = o;
                            if (nv == 4) {
                                u8_in(((v[0] << 2) | (v[1] >> 4)) & 0xFF);
                                u8_in(((v[1] << 4) | (v[2] >> 2)) & 0xFF);
                                u8_in(((v[2] << 6) | v[3]) & 0xFF);
                                nv = 0;
                            }
                        }
                        if (nv >= 2) u8_in(((v[0] << 2) | (v[1] >> 4)) & 0xFF);
                        if (nv == 3) u8_in(((v[1] << 4) | (v[2] >> 2)) & 0xFF);
                    } else {
                        for (k = start; k < end; k++) {
                            if (in[k] == '_') u8_in(' ');
                            else if (in[k] == '=' && hexv(in[k + 1]) >= 0 && hexv(in[k + 2]) >= 0) {
                                u8_in((unsigned)((hexv(in[k + 1]) << 4) | hexv(in[k + 2]))); k += 2;
                            } else u8_in((unsigned char)in[k]);
                        }
                    }
                    g_cs_utf8 = 0;
                    i = end + 2; prevenc = 1;
                    continue;
                }
            }
        }
        if (prevenc && (in[i] == ' ' || in[i] == '\t')) {
            k = i;
            while (in[k] == ' ' || in[k] == '\t') k++;
            if (in[k] == '=' && in[k + 1] == '?') { i = k; continue; }
        }
        prevenc = 0;
        g_mwp[g_mwo++] = in[i++];
    }
    g_mwp[g_mwo] = 0;
    g_sink = 0;
}

static void trim_both(char FAR *s)
{
    int n = s_len(s), i = 0, k;
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (i) { for (k = 0; s[i + k]; k++) s[k] = s[i + k]; s[k] = 0; }
}

static void parse_addr(const char FAR *h, char FAR *name, int nmax, char FAR *addr, int amax)
{
    int i, lt = -1, gt = -1, n;
    name[0] = 0; addr[0] = 0; g_tmpb[0] = 0;
    for (i = 0; h[i]; i++) {
        if (h[i] == '<' && lt < 0) lt = i;
        else if (h[i] == '>' && lt >= 0 && gt < 0) gt = i;
    }
    if (lt >= 0 && gt > lt) {
        n = 0;
        for (i = lt + 1; i < gt && n < amax - 1; i++) addr[n++] = h[i];
        addr[n] = 0;
        n = 0;
        for (i = 0; i < lt && n < (int)sizeof(g_tmpb) - 1; i++) g_tmpb[n++] = h[i];
        g_tmpb[n] = 0;
    } else {
        n = 0;
        for (i = 0; h[i] && h[i] != ' ' && h[i] != '(' && n < amax - 1; i++) addr[n++] = h[i];
        addr[n] = 0;
        for (; h[i] && h[i] != '('; i++) ;
        if (h[i] == '(') {
            n = 0; i++;
            while (h[i] && h[i] != ')' && n < (int)sizeof(g_tmpb) - 1) g_tmpb[n++] = h[i++];
            g_tmpb[n] = 0;
        }
    }
    trim_both(addr);
    trim_both(g_tmpb);
    n = s_len(g_tmpb);
    if (n >= 2 && g_tmpb[0] == '"' && g_tmpb[n - 1] == '"') { g_tmpb[n - 1] = 0; for (i = 0; g_tmpb[i + 1]; i++) g_tmpb[i] = g_tmpb[i + 1]; g_tmpb[i] = 0; }
    mime_words(g_tmpb, name, nmax);
    trim_both(name);
    if (!name[0]) s_cpy(name, addr, nmax);
}

/* ---------- header parsing ---------- */
static int get_param(const char FAR *val, const char FAR *pname, char FAR *out, int max)
{
    int i, j, k, pl = s_len(pname);
    for (i = 0; val[i]; i++) {
        if (i > 0 && val[i - 1] != ';' && val[i - 1] != ' ' && val[i - 1] != '\t') continue;
        for (k = 0; k < pl && lc((unsigned char)val[i + k]) == lc((unsigned char)pname[k]); k++) ;
        if (k != pl) continue;
        j = i + pl;
        while (val[j] == ' ') j++;
        if (val[j] != '=') continue;
        j++;
        while (val[j] == ' ') j++;
        k = 0;
        if (val[j] == '"') { j++; while (val[j] && val[j] != '"' && k < max - 1) out[k++] = val[j++]; }
        else { while (val[j] && val[j] != ';' && val[j] != ' ' && k < max - 1) out[k++] = val[j++]; }
        out[k] = 0;
        return 1;
    }
    return 0;
}

static void first_token_lc(char FAR *dst, int max, const char FAR *val)
{
    int i;
    for (i = 0; val[i] && val[i] != ';' && val[i] != ' ' && i < max - 1; i++)
        dst[i] = (char)lc((unsigned char)val[i]);
    dst[i] = 0;
}

static void apply_header(ENTH FAR *e, int top, const char FAR *name, const char FAR *val)
{
    if (s_icmp(name, "Content-Type") == 0) {
        first_token_lc(e->type, sizeof(e->type), val);
        get_param(val, "boundary", e->boundary, sizeof(e->boundary));
        get_param(val, "charset", e->charset, sizeof(e->charset));
        if (!e->fname[0]) get_param(val, "name", e->fname, sizeof(e->fname));
    } else if (s_icmp(name, "Content-Transfer-Encoding") == 0) {
        first_token_lc(e->cte, sizeof(e->cte), val);
    } else if (s_icmp(name, "Content-Disposition") == 0) {
        first_token_lc(e->disp, sizeof(e->disp), val);
        get_param(val, "filename", e->fname, sizeof(e->fname));
    } else if (top) {
        if (s_icmp(name, "From") == 0) s_cpy(g_in_from, val, sizeof(g_in_from));
        else if (s_icmp(name, "Subject") == 0) s_cpy(g_in_subj, val, sizeof(g_in_subj));
        else if (s_icmp(name, "Date") == 0) s_cpy(g_in_date, val, sizeof(g_in_date));
        else if (s_icmp(name, "To") == 0) s_cpy(g_in_to, val, sizeof(g_in_to));
        else if (s_icmp(name, "Cc") == 0) s_cpy(g_in_cc, val, sizeof(g_in_cc));
    }
}

static void read_headers(ENTH FAR *e, int top)
{
    int n, have = 0, i;
    char FAR *p;
    for (;;) {
        n = rd_line(g_line, sizeof(g_line));
        if (n < 0) {
            if (have) apply_header(e, top, g_hn, g_hv);
            g_end_level = -1;
            return;
        }
        if (n == 0) {
            if (have) apply_header(e, top, g_hn, g_hv);
            return;
        }
        if (g_line[0] == ' ' || g_line[0] == '\t') {
            if (have) {
                i = 0;
                while (g_line[i] == ' ' || g_line[i] == '\t') i++;
                s_cat(g_hv, " ", sizeof(g_hv));
                s_cat(g_hv, g_line + i, sizeof(g_hv));
            }
            continue;
        }
        if (have) apply_header(e, top, g_hn, g_hv);
        have = 0;
        for (i = 0; g_line[i] && g_line[i] != ':'; i++) ;
        if (!g_line[i]) continue;
        g_line[i] = 0;
        s_cpy(g_hn, g_line, sizeof(g_hn));
        p = g_line + i + 1;
        while (*p == ' ' || *p == '\t') p++;
        s_cpy(g_hv, p, sizeof(g_hv));
        have = 1;
    }
}

/* ---------- MIME entity walker ---------- */
static int is_delim(const char FAR *ln)
{
    int k, i;
    if (ln[0] != '-' || ln[1] != '-') return 0;
    for (k = g_nb - 1; k >= 0; k--) {
        i = 0;
        while (g_bnd[k][i] && ln[2 + i] == g_bnd[k][i]) i++;
        if (g_bnd[k][i] != 0) continue;
        if (ln[2 + i] == 0 || ln[2 + i] == ' ' || ln[2 + i] == '\t') { g_end_level = k; g_end_close = 0; return 1; }
        if (ln[2 + i] == '-' && ln[3 + i] == '-') { g_end_level = k; g_end_close = 1; return 1; }
    }
    return 0;
}

static void skip_to_delim(void)
{
    for (;;) {
        if (rd_line(g_line, sizeof(g_line)) < 0) { g_end_level = -1; return; }
        if (is_delim(g_line)) return;
    }
}

static void body_loop(ENTH FAR *e, int mode)
{
    int cte = 0, n;
    if (s_icmp(e->cte, "base64") == 0) cte = 1;
    else if (s_icmp(e->cte, "quoted-printable") == 0) cte = 2;
    g_mode = mode; g_bn = 0; g_u8need = 0; html_reset();
    g_cs_utf8 = s_icontains(e->charset, "utf-8") || s_icontains(e->charset, "utf8");
    g_sink = 0;
    for (;;) {
        n = rd_line(g_line, sizeof(g_line));
        if (n < 0) { g_end_level = -1; break; }
        if (is_delim(g_line)) break;
        decode_line(g_line, n, cte);
    }
    g_cs_utf8 = 0;
}

static void entity(int depth, int top)
{
    ENTH FAR *e = &g_enth[depth];
    int level, is_text, is_html, attach, mode;
    char nm[8];

    e->type[0] = e->boundary[0] = e->charset[0] = e->cte[0] = e->fname[0] = e->disp[0] = 0;
    read_headers(e, top);
    if (e->type[0] == 0) s_cpy(e->type, "text/plain", sizeof(e->type));
    if (starts_ci(e->type, "multipart/") && !e->boundary[0]) s_cpy(e->type, "text/plain", sizeof(e->type));

    if (starts_ci(e->type, "multipart/") && e->boundary[0] && g_nb < 4 && depth < 5) {
        level = g_nb;
        s_cpy(g_bnd[g_nb++], e->boundary, sizeof(g_bnd[0]));
        skip_to_delim();
        while (g_end_level == level && !g_end_close) entity(depth + 1, 0);
        g_nb--;
        if (g_end_level == level) skip_to_delim();
        return;
    }

    is_text = starts_ci(e->type, "text/");
    is_html = starts_ci(e->type, "text/html");
    attach = (e->fname[0] || starts_ci(e->disp, "attachment") || !is_text);
    if (!attach && !g_have_body) mode = is_html ? M_HTML : M_TEXT;
    else if (!attach) mode = M_SKIP;
    else if (g_natt < MAXATT) mode = M_ATTACH;
    else mode = M_SKIP;

    if (mode == M_TEXT || mode == M_HTML) {
        if (out_open(g_msgout)) mode = M_SKIP;
    } else if (mode == M_ATTACH) {
        g_tmpa[0] = 0;
        s_cat(g_tmpa, g_indir, sizeof(g_tmpa)); s_cat(g_tmpa, "\\", sizeof(g_tmpa));
        s_cat(g_tmpa, g_base, sizeof(g_tmpa)); s_cat(g_tmpa, ".", sizeof(g_tmpa));
        nm[0] = (char)('0' + (g_natt / 100) % 10); nm[1] = (char)('0' + (g_natt / 10) % 10);
        nm[2] = (char)('0' + g_natt % 10); nm[3] = 0;
        s_cat(g_tmpa, nm, sizeof(g_tmpa));
        if (out_open(g_tmpa)) mode = M_SKIP;
        else {
            s_cpy(g_attp[g_natt], g_tmpa, sizeof(g_attp[0]));
            if (e->fname[0]) mime_words(e->fname, g_attn[g_natt], sizeof(g_attn[0]));
            else { g_attn[g_natt][0] = 0; s_cat(g_attn[g_natt], "attachment.", sizeof(g_attn[0])); s_cat(g_attn[g_natt], nm, sizeof(g_attn[0])); }
            g_natt++;
        }
    }
    body_loop(e, mode);
    out_close();
    if (mode == M_TEXT || mode == M_HTML) g_have_body = 1;
}

/* ---------- date conversion ---------- */
static const char *MONTHS[] = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
static const char *WDAYS[]  = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };

static int days_in(int y, int m)
{
    static const char D[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return D[m - 1];
}

/* "Tue, 3 Nov 2026 15:20:11 -0500"  ->  "Tue Nov 03, 2026  08:20 pm GMT" */
static int date_to_mci(const char FAR *in, char FAR *out, int max)
{
    int day = 0, mon = 0, year = 0, hh = -1, mi = 0, off = 0, have_off = 0;
    int i, j, k, mins, wd, pm;
    char tok[24], t[8];
    static const char T[] = { 0,3,2,5,0,3,5,1,4,6,2,4 };

    i = 0;
    while (in[i]) {
        while (in[i] == ' ' || in[i] == ',' || in[i] == '\t') i++;
        j = 0;
        while (in[i] && in[i] != ' ' && in[i] != ',' && in[i] != '\t' && j < 23) tok[j++] = in[i++];
        tok[j] = 0;
        if (!j) break;
        if (tok[0] >= '0' && tok[0] <= '9' && s_len(tok) <= 2 && day == 0 && tok[1] != ':' && tok[2] != ':') {
            day = tok[0] - '0'; if (tok[1]) day = day * 10 + (tok[1] - '0');
        } else if (tok[0] >= '0' && tok[0] <= '9' && (s_len(tok) == 4 || s_len(tok) == 2) && mon && !year && !s_icontains(tok, ":")) {
            year = 0; for (k = 0; tok[k]; k++) year = year * 10 + (tok[k] - '0');
            if (s_len(tok) == 2) year += (year >= 70) ? 1900 : 2000;
        } else if (s_icontains(tok, ":")) {
            hh = 0; k = 0;
            while (tok[k] && tok[k] != ':') hh = hh * 10 + (tok[k++] - '0');
            if (tok[k] == ':') { k++; mi = 0; while (tok[k] && tok[k] != ':') mi = mi * 10 + (tok[k++] - '0'); }
        } else if ((tok[0] == '+' || tok[0] == '-') && s_len(tok) == 5) {
            off = ((tok[1] - '0') * 10 + (tok[2] - '0')) * 60 + (tok[3] - '0') * 10 + (tok[4] - '0');
            if (tok[0] == '-') off = -off;
            have_off = 1;
        } else {
            for (k = 0; k < 12; k++) if (starts_ci(tok, MONTHS[k]) && s_len(tok) >= 3) { mon = k + 1; break; }
            if (!mon || k == 12) {
                if (s_icmp(tok, "GMT") == 0 || s_icmp(tok, "UT") == 0 || s_icmp(tok, "UTC") == 0 || s_icmp(tok, "Z") == 0) { off = 0; have_off = 1; }
                else if (s_icmp(tok, "EST") == 0) { off = -300; have_off = 1; }
                else if (s_icmp(tok, "EDT") == 0) { off = -240; have_off = 1; }
                else if (s_icmp(tok, "CST") == 0) { off = -360; have_off = 1; }
                else if (s_icmp(tok, "CDT") == 0) { off = -300; have_off = 1; }
                else if (s_icmp(tok, "MST") == 0) { off = -420; have_off = 1; }
                else if (s_icmp(tok, "MDT") == 0) { off = -360; have_off = 1; }
                else if (s_icmp(tok, "PST") == 0) { off = -480; have_off = 1; }
                else if (s_icmp(tok, "PDT") == 0) { off = -420; have_off = 1; }
            }
        }
    }
    (void)have_off;
    if (!day || !mon || !year || hh < 0) return 0;

    mins = hh * 60 + mi - off;
    while (mins < 0)     { mins += 1440; day--; if (day < 1) { mon--; if (mon < 1) { mon = 12; year--; } day = days_in(year, mon); } }
    while (mins >= 1440) { mins -= 1440; day++; if (day > days_in(year, mon)) { day = 1; mon++; if (mon > 12) { mon = 1; year++; } } }
    hh = mins / 60; mi = mins % 60;

    { int y = year; if (mon < 3) y--; wd = (y + y / 4 - y / 100 + y / 400 + T[mon - 1] + day) % 7; }
    pm = (hh >= 12);
    k = hh % 12; if (k == 0) k = 12;

    out[0] = 0;
    s_cat(out, WDAYS[wd], max); s_cat(out, " ", max); s_cat(out, MONTHS[mon - 1], max); s_cat(out, " ", max);
    put2(t, day); t[2] = 0; s_cat(out, t, max); s_cat(out, ", ", max);
    fmt_uint(t, year); s_cat(out, t, max); s_cat(out, "  ", max);
    put2(t, k); t[2] = 0; s_cat(out, t, max); s_cat(out, ":", max);
    put2(t, mi); t[2] = 0; s_cat(out, t, max); s_cat(out, pm ? " pm" : " am", max);
    s_cat(out, " GMT", max);
    return 1;
}

/* ---------- POP3 ---------- */
static int pop_resp(char FAR *line, int max)         /* 1 = +OK, 0 = -ERR, -1 = error */
{
    int n = net_line(line, max);
    if (n < 0) { s_cpy(g_reply, "connection lost or timed out", sizeof(g_reply)); return -1; }
    s_cpy(g_reply, line, sizeof(g_reply));
    tr2("S: ", line);
    return (line[0] == '+') ? 1 : 0;
}

static int pop_cmd(const char FAR *cmd, const char FAR *shown)
{
    int r;
    tr2("C: ", shown ? shown : cmd);
    if (net_puts(cmd) || net_puts("\r\n") || net_flush()) {
        s_cpy(g_reply, "send failed", sizeof(g_reply));
        g_pop_dead = 1;
        return -1;
    }
    r = pop_resp(g_line, sizeof(g_line));
    if (r < 0) g_pop_dead = 1;
    return r;
}

static void pop_quit(void)
{
    if (g_s != BADSOCK) pop_cmd("QUIT", 0);
    net_close();
    g_pop_open = 0;
}

static int next_id(char FAR *out, int max)
{
    int n = 0;
    while (g_pop_users[g_upos] == ',' || g_pop_users[g_upos] == ' ' ||
           g_pop_users[g_upos] == ';' || g_pop_users[g_upos] == '\t') g_upos++;
    while (g_pop_users[g_upos] && g_pop_users[g_upos] != ',' && g_pop_users[g_upos] != ' ' &&
           g_pop_users[g_upos] != ';' && g_pop_users[g_upos] != '\t') {
        if (n < max - 1) out[n++] = g_pop_users[g_upos];
        g_upos++;
    }
    out[n] = 0;
    return n > 0;
}

static int pop_open_user(const char FAR *id)         /* 1 = mailbox open (g_pop_n set) */
{
    char sect[80], cmd[120];
    int r, i;

    sect[0] = 0; s_cat(sect, "USER.", sizeof(sect)); s_cat(sect, id, sizeof(sect));
    GetPrivateProfileString(sect, "User", "", g_pop_user, sizeof(g_pop_user), g_ini);
    GetPrivateProfileString(sect, "Password", "", g_pop_pass, sizeof(g_pop_pass), g_ini);
    cfg_override(sect, "PopUser", g_pop_user, sizeof(g_pop_user));
    cfg_override(sect, "PopPassword", g_pop_pass, sizeof(g_pop_pass));
    GetPrivateProfileString(sect, "FullName", id, g_pop_name, sizeof(g_pop_name), g_ini);
    GetPrivateProfileString(sect, "Email", "", g_pop_email, sizeof(g_pop_email), g_ini);
    s_cpy(g_pop_id, id, sizeof(g_pop_id));

    tr2("Mailbox for user id ", id);
    if (!g_pop_user[0]) { errlog2("POP3: no User= in section ", sect); return 0; }

    g_pop_dead = 0;
    if (net_open(g_phost, g_pport)) { errlog2("POP3: ", g_reply); return 0; }
    r = pop_resp(g_line, sizeof(g_line));
    if (r != 1) { errlog2("POP3 greeting: ", g_reply); net_close(); return 0; }

    cmd[0] = 0; s_cat(cmd, "USER ", sizeof(cmd)); s_cat(cmd, g_pop_user, sizeof(cmd));
    if (pop_cmd(cmd, 0) != 1) { errlog2("POP3 USER refused: ", g_reply); net_close(); return 0; }
    cmd[0] = 0; s_cat(cmd, "PASS ", sizeof(cmd)); s_cat(cmd, g_pop_pass, sizeof(cmd));
    if (pop_cmd(cmd, "PASS ********") != 1) { errlog2("POP3 login failed: ", g_reply); net_close(); return 0; }

    if (pop_cmd("STAT", 0) != 1) { errlog2("POP3 STAT failed: ", g_reply); pop_quit(); return 0; }
    g_pop_n = 0;
    for (i = 3; g_line[i] == ' '; i++) ;
    while (g_line[i] >= '0' && g_line[i] <= '9') { g_pop_n = g_pop_n * 10 + (g_line[i] - '0'); i++; }
    g_pop_i = g_newest ? g_pop_n : 1;
    g_pop_step = g_newest ? -1 : 1;
    g_pop_open = 1;
    return 1;
}

static int pop_retr(int n, const char FAR *path)      /* message -> raw file */
{
    char cmd[24], num[8];
    int len;
    const char FAR *t;

    fmt_uint(num, (unsigned)n);
    cmd[0] = 0; s_cat(cmd, "RETR ", sizeof(cmd)); s_cat(cmd, num, sizeof(cmd));
    if (pop_cmd(cmd, 0) != 1) { errlog2("POP3 RETR failed: ", g_reply); return -1; }
    if (out_open(path)) { errlog2("Cannot create ", path); return -1; }
    for (;;) {
        len = net_line(g_line, sizeof(g_line));
        if (len < 0) { out_close(); g_pop_dead = 1; errlog("POP3: connection lost while receiving a message"); return -1; }
        if (g_line[0] == '.' && g_line[1] == 0) break;
        t = g_line;
        if (g_line[0] == '.') t++;
        out_puts(t); out_put('\r'); out_put('\n');
    }
    out_close();
    return 0;
}

/* ---------- build ENV file ---------- */
static void env_line(const char FAR *key, const char FAR *val)
{
    out_puts(key); out_put(':');
    while (*val) { out_put(((unsigned char)*val < 32) ? ' ' : (unsigned char)*val); val++; }
    out_put('\r'); out_put('\n');
}

static void make_base(void)
{
    union { DWORD l; WORD w[2]; } u;
    char h[6], p[160];
    int tries;
    u.l = GetTickCount();
    if (g_bufbase[0]) {                          /* the name Bob chose for this message */
        p[0] = 0; s_cat(p, g_indir, sizeof(p)); s_cat(p, "\\", sizeof(p));
        s_cat(p, g_bufbase, sizeof(p)); s_cat(p, ".ENV", sizeof(p));
        if (!file_exists(p)) { s_cpy(g_base, g_bufbase, sizeof(g_base)); return; }
        tr2("Name already in use, choosing another: ", g_bufbase);
    }
    for (tries = 0; tries < 200; tries++) {
        g_fcount++;
        g_base[0] = 'R'; g_base[1] = 'M';
        hex4(g_base + 2, u.w[0] + g_fcount * 7);
        g_base[6] = (char)("0123456789abcdef"[(g_fcount >> 4) & 15]);
        g_base[7] = (char)("0123456789abcdef"[g_fcount & 15]);
        g_base[8] = 0;
        p[0] = 0; s_cat(p, g_indir, sizeof(p)); s_cat(p, "\\", sizeof(p));
        s_cat(p, g_base, sizeof(p)); s_cat(p, ".ENV", sizeof(p));
        if (!file_exists(p)) return;
    }
    (void)h;
}

static char g_rn[120], g_ra[120], g_item[200];

static int next_item(const char FAR *list, int FAR *pos, char FAR *item, int max)   /* next address in a list */
{
    int n = 0, inq = 0, inang = 0, i = *pos;
    char c;
    while (list[i] == ' ' || list[i] == ',' || list[i] == '\t') i++;
    if (!list[i]) { *pos = i; return 0; }
    while (list[i]) {
        c = list[i];
        if (c == '"') inq = !inq;
        else if (!inq) {
            if (c == '<') inang = 1;
            else if (c == '>') inang = 0;
            else if (c == ',' && !inang) break;
        }
        if (n < max - 1) item[n++] = c;
        i++;
    }
    item[n] = 0;
    *pos = i;
    return 1;
}

static void env_rcpt_list(const char FAR *key, const char FAR *list, int FAR *count)
{
    int pos = 0;
    while (*count < 6 && next_item(list, &pos, g_item, sizeof(g_item))) {
        parse_addr(g_item, g_rn, sizeof(g_rn), g_ra, sizeof(g_ra));
        if (!g_ra[0] || !s_icontains(g_ra, "@")) continue;
        if (g_pop_email[0] && s_icmp(g_ra, g_pop_email) == 0) continue;   /* the Bob user is listed first */
        env_line(key, g_rn); env_line("EMS", "INTERNET"); env_line("MBX", g_ra);
        (*count)++;
    }
}

static void write_env(const char FAR *envpath)
{
    char name[120], addr[120], datebuf[80];
    int i;

    parse_addr(g_in_from, name, sizeof(name), addr, sizeof(addr));
    if (!addr[0]) { s_cpy(addr, "unknown@unknown", sizeof(addr)); if (!name[0]) s_cpy(name, "Unknown", sizeof(name)); }

    out_open(envpath);
    env_line("From", name);
    env_line("EMS", "INTERNET");
    env_line("MBX", addr);
    env_line("TO", g_pop_name);
    env_line("EMS", "MCI Mail");
    env_line("MBX", g_pop_id);
    if (g_showrcpt) {
        int cnt = 0;
        env_rcpt_list("TO", g_in_to, &cnt);
        env_rcpt_list("Cc", g_in_cc, &cnt);
    }
    mime_words(g_in_subj, g_tmpa, sizeof(g_tmpa));
    env_line("Subject", g_tmpa);
    if (s_icmp(g_datestyle, "mci") == 0) {
        if (date_to_mci(g_in_date, datebuf, sizeof(datebuf))) env_line("Date", datebuf);
    } else if (s_icmp(g_datestyle, "rfc") == 0) {
        if (g_in_date[0]) env_line("Date", g_in_date);
    }
    for (i = 0; i < g_natt; i++) {
        env_line("AttDesc", g_attn[i]);
        env_line("Detach", g_attp[i]);
    }
    out_close();
}

static void keep_raw(const char FAR *src, const char FAR *base)   /* copy raw mail to <mailroom>\\base.EML */
{
    char dst[180];
    int hi, ho;
    UINT n;
    dst[0] = 0; s_cat(dst, g_dir, sizeof(dst)); s_cat(dst, "\\", sizeof(dst));
    s_cat(dst, base, sizeof(dst)); s_cat(dst, ".EML", sizeof(dst));
    hi = _lopen(src, OF_READ | OF_SHARE_DENY_NONE);
    if (hi == -1) return;
    ho = _lcreat(dst, 0);
    if (ho == -1) { _lclose(hi); tr2("KeepRaw: cannot create ", dst); return; }
    for (;;) {
        n = _lread(hi, g_io, sizeof(g_io));
        if (n == 0 || n == (UINT)-1) break;
        _lwrite(ho, (LPCSTR)g_io, (int)n);
    }
    _lclose(hi); _lclose(ho);
    tr2("Raw copy kept as ", dst);
}

static void trace_env(const char FAR *path)     /* copy the envelope's lines into the trace */
{
    int h, k = 0;
    UINT n, i;
    char ln[140];
    h = _lopen(path, OF_READ | OF_SHARE_DENY_NONE);
    if (h == -1) return;
    n = _lread(h, g_io, sizeof(g_io));
    _lclose(h);
    if (n == (UINT)-1) return;
    for (i = 0; i < n; i++) {
        if (g_io[i] == '\n') { ln[k] = 0; tr2("ENV> ", ln); k = 0; }
        else if (g_io[i] != '\r' && k < 130) ln[k++] = (char)g_io[i];
    }
    if (k) { ln[k] = 0; tr2("ENV> ", ln); }
}

static int fetch_one(int n)
{
    char tmp[180], envp[180], num[8];
    int i;

    g_have_body = 0; g_natt = 0; g_nb = 0; g_end_level = -1; g_end_close = 0;
    g_in_from[0] = g_in_subj[0] = g_in_date[0] = g_in_to[0] = g_in_cc[0] = 0;

    tmp[0] = 0; s_cat(tmp, g_dir, sizeof(tmp)); s_cat(tmp, "\\RECV.TMP", sizeof(tmp));
    if (pop_retr(n, tmp)) { del_file(tmp); return 0; }

    make_base();
    g_msgout[0] = 0; s_cat(g_msgout, g_indir, sizeof(g_msgout)); s_cat(g_msgout, "\\", sizeof(g_msgout));
    s_cat(g_msgout, g_base, sizeof(g_msgout)); s_cat(g_msgout, ".MSG", sizeof(g_msgout));
    envp[0] = 0; s_cat(envp, g_indir, sizeof(envp)); s_cat(envp, "\\", sizeof(envp));
    s_cat(envp, g_base, sizeof(envp)); s_cat(envp, ".ENV", sizeof(envp));

    if (out_open(g_msgout)) { errlog2("Cannot create ", g_msgout); del_file(tmp); return 0; }
    out_close();

    g_rf = _lopen(tmp, OF_READ | OF_SHARE_DENY_NONE);
    if (g_rf == -1) { errlog2("Cannot read ", tmp); del_file(g_msgout); return 0; }
    g_rdn = g_rdp = 0;
    entity(0, 1);
    _lclose(g_rf); g_rf = -1;
    out_close();
    if (g_keepraw) keep_raw(tmp, g_base);
    del_file(tmp);

    write_env(envp);
    trace_env(envp);
    fmt_uint(num, (unsigned)n);
    tr2("Stored message ", num);
    tr2("  as ", envp);
    if (g_delete) {
        char cmd[24];
        cmd[0] = 0; s_cat(cmd, "DELE ", sizeof(cmd)); s_cat(cmd, num, sizeof(cmd));
        if (pop_cmd(cmd, 0) != 1) errlog2("POP3 DELE failed: ", g_reply);
    }
    (void)i;
    return 1;
}

static void set_target(const char FAR *buf)
{
    /* Bob passes "<IN folder>\\<unique name>": the engine must create
       <that name>.ENV / .MSG / .000 ...  (a plain folder name also works). */
    int i, last = -1, n;
    g_bufbase[0] = 0;
    s_cpy(g_indir, g_cfgdir, sizeof(g_indir));
    if (buf == 0 || IsBadReadPtr(buf, 4) || buf[1] != ':' || buf[2] != '\\') return;
    for (i = 0; buf[i] && buf[i] != '*'; i++) if (buf[i] == '\\') last = i;
    if (last < 3) return;
    n = 0;
    for (i = last + 1; buf[i] && buf[i] != '.' && buf[i] != '*' && n < 8; i++) g_bufbase[n++] = buf[i];
    g_bufbase[n] = 0;
    if (n == 0 || s_icmp(g_bufbase, "IN") == 0) {          /* it was just the folder */
        g_bufbase[0] = 0;
        n = 0;
        for (i = 0; buf[i] && buf[i] != '*' && n < (int)sizeof(g_indir) - 1; i++) g_indir[n++] = buf[i];
        g_indir[n] = 0;
        while (n > 3 && g_indir[n - 1] == '\\') g_indir[--n] = 0;
        return;
    }
    n = 0;
    for (i = 0; i < last && n < (int)sizeof(g_indir) - 1; i++) g_indir[n++] = buf[i];
    g_indir[n] = 0;
}

static int recv_cfg(void)
{
    int n, i;
    GetPrivateProfileString("SERVER", "Users", "", g_pop_users, sizeof(g_pop_users), g_ini);
    GetPrivateProfileString("SERVER", "Pop3Host", "msbob.org", g_phost, sizeof(g_phost), g_ini);
    g_pport = GetPrivateProfileInt("SERVER", "Pop3Port", 110, g_ini);
    g_delete = GetPrivateProfileInt("SERVER", "Delete", 1, g_ini);
    g_keepraw = GetPrivateProfileInt("SERVER", "KeepRaw", 1, g_ini);
    g_showrcpt = (int)GetPrivateProfileInt("SERVER", "ShowRecipients", 1, g_ini);
    g_maxrun = (int)GetPrivateProfileInt("SERVER", "MaxPerRun", 25, g_ini);
    g_newest = (int)GetPrivateProfileInt("SERVER", "NewestFirst", 0, g_ini);
    g_stored = 0;
    GetPrivateProfileString("SERVER", "DateStyle", "none", g_datestyle, sizeof(g_datestyle), g_ini);
    GetPrivateProfileString("SERVER", "InDir", "", g_cfgin, sizeof(g_cfgin), g_ini);
    g_upos = 0;

    if (g_cfgin[0]) s_cpy(g_cfgdir, g_cfgin, sizeof(g_cfgdir));
    else { g_cfgdir[0] = 0; s_cat(g_cfgdir, g_dir, sizeof(g_cfgdir)); s_cat(g_cfgdir, "\\IN", sizeof(g_cfgdir)); }
    for (i = 0; g_cfgdir[i]; i++) if (g_cfgdir[i] == '*') { g_cfgdir[i] = 0; break; }
    n = s_len(g_cfgdir);
    while (n > 3 && g_cfgdir[n - 1] == '\\') g_cfgdir[--n] = 0;

    tr2("Fallback IN folder: ", g_cfgdir);
    tr2("POP3 host=", g_phost);
    if (!g_pop_users[0]) { errlog2("No Users= line in [SERVER] of ", g_ini); return -1; }
    return 0;
}

static int dl_next(void)                    /* 1 = a message was stored, 0 = all done */
{
    char id[40];
    for (;;) {
        if (g_maxrun > 0 && g_stored >= g_maxrun) {
            if (g_pop_open) { tr1("MaxPerRun reached: more mail stays on the server for the next run"); pop_quit(); }
            return 0;
        }
        if (!g_pop_open) {
            if (!next_id(id, sizeof(id))) return 0;
            if (!pop_open_user(id)) continue;
            if (g_pop_n == 0) { tr1("mailbox is empty"); pop_quit(); continue; }
        }
        if (g_pop_i > g_pop_n || g_pop_i < 1) { pop_quit(); continue; }
        {
            int ok = fetch_one(g_pop_i);
            g_pop_i += g_pop_step;
            if (g_pop_dead) {                            /* connection died: give up on this mailbox */
                net_close(); g_pop_open = 0; g_pop_dead = 0;
                errlog("POP3: giving up on this mailbox for this run");
            }
            if (ok) { g_stored++; return 1; }
        }
    }
}

/* ------------------------------------------------------------------ */
/* exported MM* API  (argument byte counts verified against the        */
/* original UMLMM.DLL's "retf N" epilogues)                            */
/* ------------------------------------------------------------------ */
/* N=12 */
MMAPI MMOpenSession(LPSTR app, WORD ver, WORD mode, DWORD FAR *ph)
{
    int h;
    (void)app; (void)ver; (void)mode;
    h = _lcreat(g_tracef, 0);               /* start a fresh trace each run */
    if (h != -1) _lclose(h);
    tr1("--- UMLMM gateway: mail run started ---");
    reset_msg();
    g_dl_state = 0; g_pop_open = 0;
    if (ph != 0 && !IsBadWritePtr(ph, 4)) *ph = FAKE_SESSION;
    return 0;                               /* 0 = success for this call */
}

/* N=4 */
MMAPI MMCloseSession(DWORD h)
{
    (void)h;
    tr1("session closed");
    return MM_OK;
}

/* N=12 : path of MAILRUN.LOG; the mailroom folder is derived from it */
MMAPI MMSetLogFile(DWORD h, LPSTR path, WORD a, WORD b)
{
    int i, last = -1;
    (void)h; (void)a; (void)b;
    if (path != 0 && !IsBadReadPtr(path, 4)) {
        s_cpy(g_mrlog, path, sizeof(g_mrlog));
        for (i = 0; g_mrlog[i]; i++) if (g_mrlog[i] == '\\') last = i;
        if (last > 0) {
            for (i = 0; i < last && i < (int)sizeof(g_dir) - 1; i++) g_dir[i] = g_mrlog[i];
            g_dir[i] = 0;
            g_tracef[0] = 0; s_cat(g_tracef, g_dir, sizeof(g_tracef)); s_cat(g_tracef, "\\UMLMM.TRC", sizeof(g_tracef));
            g_ini[0] = 0;    s_cat(g_ini, g_dir, sizeof(g_ini));       s_cat(g_ini, "\\POP3.INI", sizeof(g_ini));
        }
    }
    tr2("config file: ", g_ini);
    return MM_OK;
}

/* N=14 */
MMAPI MMSetMeter(DWORD h, FARPROC cb, LPVOID user, WORD n)
{
    (void)h; (void)cb; (void)user; (void)n;
    return MM_OK;
}

/* N=12 */
MMAPI MMSetService(DWORD h, WORD one, LPSTR svc, WORD z)
{
    (void)h; (void)one; (void)svc; (void)z;
    return MM_OK;
}

/* N=12 */
MMAPI MMConfigureAccount(DWORD h, LPSTR id, LPSTR pw)
{
    (void)h; (void)id; (void)pw;
    return MM_OK;
}

/* N=26 */
MMAPI MMConfigureBoxes(WORD a, WORD b, WORD c, WORD d, WORD e, WORD f,
                       WORD g, WORD h, WORD i, WORD j, WORD k, WORD l, WORD m)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g;
    (void)h; (void)i; (void)j; (void)k; (void)l; (void)m;
    return MM_OK;
}

/* N=26 */
MMAPI MMConfigureConnect(WORD a, WORD b, WORD c, WORD d, WORD e, WORD f,
                         WORD g, WORD h, WORD i, WORD j, WORD k, WORD l, WORD m)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g;
    (void)h; (void)i; (void)j; (void)k; (void)l; (void)m;
    return MM_OK;
}

/* N=14 */
MMAPI MMConfigureHandling(DWORD h, WORD a, WORD b, WORD c, WORD d, WORD e)
{
    (void)h; (void)a; (void)b; (void)c; (void)d; (void)e;
    return MM_OK;
}

/* N=14 */
MMAPI MMConfigureMail(WORD a, WORD b, WORD c, WORD d, WORD e, WORD f, WORD g)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g;
    return MM_OK;
}

/* N=8 */
MMAPI MMConfigureMcd(WORD a, WORD b, WORD c, WORD d)
{
    (void)a; (void)b; (void)c; (void)d;
    return MM_OK;
}

/* N=20 */
MMAPI MMConfigureModem(WORD a, WORD b, WORD c, WORD d, WORD e,
                       WORD f, WORD g, WORD h, WORD i, WORD j)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; (void)j;
    return MM_OK;
}

/* N=16 */
MMAPI MMConfigurePort(WORD a, WORD b, WORD c, WORD d,
                      WORD e, WORD f, WORD g, WORD h)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h;
    return MM_OK;
}

/* N=12 */
MMAPI MMConfigureTranscript(DWORD h, LPVOID p, WORD a, WORD b)
{
    (void)h; (void)p; (void)a; (void)b;
    return MM_OK;
}

/* N=4 : "connect" = bring WinSock up (the real connection is made per message) */
MMAPI MMConnect(DWORD h)
{
    char wsadata[512];
    (void)h;
    if (!g_wsup) {
        if (ws_startup(0x0101, wsadata) != 0) {
            errlog("WinSock could not be started");
            return MM_FAIL;
        }
        g_wsup = 1;
    }
    tr1("WinSock started");
    return MM_OK;
}

/* N=14 : nothing to verify yet; real login happens against the SMTP server */
MMAPI MMLogin(DWORD h, LPSTR id, LPSTR pw, WORD z)
{
    (void)h; (void)id; (void)pw; (void)z;
    return MM_OK;
}

/* N=4 */
MMAPI MMDisconnect(DWORD h)
{
    (void)h;
    if (g_pop_open) pop_quit();
    net_close();
    if (g_wsup) { ws_cleanup(); g_wsup = 0; }
    tr1("WinSock stopped");
    return MM_OK;
}

/* N=14 : one call = one message.  100 = message stored in IN, 110 = no more.
 * "buf" = "<IN folder>\\<unique name>": files are created under that name.   */
MMAPI MMDownloadMessage(DWORD h, LPSTR buf, WORD a, WORD b, WORD c)
{
    (void)h; (void)a; (void)b; (void)c;
    if (g_dl_state == 2) return MM_NOMORE;
    if (g_dl_state == 0) {
        g_dl_state = 1;
        if (recv_cfg()) { g_dl_state = 2; return MM_NOMORE; }
    }
    if (buf != 0 && !IsBadReadPtr(buf, 4)) tr2("Download target: ", buf);
    set_target(buf);
    if (dl_next()) return MM_OK;
    g_dl_state = 2;
    return MM_NOMORE;
}

/* N=14 : envfile = path of NNNNNN.ENV */
MMAPI MMUploadEnvelope(DWORD h, LPSTR envfile, LPSTR rejfile, WORD z)
{
    (void)h; (void)rejfile; (void)z;
    reset_msg();
    tr2("Envelope: ", envfile);
    if (envfile == 0 || IsBadReadPtr(envfile, 4)) { errlog("Bad envelope file pointer"); return MM_FAIL; }
    if (parse_env(envfile)) return MM_FAIL;
    return MM_OK;
}

/* N=14 : msgfile = path of NNNNNN.MSG */
MMAPI MMUploadText(DWORD h, LPSTR msgfile, WORD a, WORD b, WORD c)
{
    (void)h; (void)a; (void)b; (void)c;
    if (msgfile == 0 || IsBadReadPtr(msgfile, 4)) { errlog("Bad message text pointer"); return MM_FAIL; }
    s_cpy(g_msgpath, msgfile, sizeof(g_msgpath));
    tr2("Text: ", g_msgpath);
    return MM_OK;
}

/* N=14 : attachments are taken from the AttDesc/Detach lines of the envelope */
MMAPI MMUploadAttachment(DWORD h, LPSTR a, LPSTR b, WORD z)
{
    (void)h; (void)b; (void)z;
    if (a != 0 && !IsBadReadPtr(a, 4)) tr2("Attachment: ", a);
    return MM_OK;
}

/* N=10 : deliver the message now */
MMAPI MMUploadSend(DWORD h, WORD a, WORD b, WORD c)
{
    int rc;
    (void)h; (void)a; (void)b; (void)c;
    rc = smtp_deliver();
    reset_msg();
    return rc ? rc : MM_OK;
}

/* N=4 */
MMAPI MMUploadAbort(DWORD h)
{
    (void)h;
    net_close();
    reset_msg();
    tr1("upload aborted");
    return MM_OK;
}

/* ------------------------------------------------------------------ */
/* DLL plumbing                                                        */
/* ------------------------------------------------------------------ */
int FAR PASCAL LibMain(HINSTANCE hInst, WORD wDataSeg, WORD wHeapSize,
                       LPSTR lpszCmdLine)
{
    (void)hInst; (void)wDataSeg; (void)wHeapSize; (void)lpszCmdLine;
    return 1;
}

int FAR PASCAL __export WEP(int nExitType)
{
    (void)nExitType;
    return 1;
}
