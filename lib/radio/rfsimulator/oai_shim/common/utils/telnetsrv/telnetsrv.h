#pragma once

#define TELNET_CMD_MAXSIZE   32
#define TELNET_HELPSTR_SIZE 128
#define TELNET_MAXLINE_NUM  100
#define TELNET_MAXCOL_NUM    10
#define CMDSTATUS_NOCMD       0
#define CMDSTATUS_EXIT        1
#define CMDSTATUS_FOUND       2
#define CMDSTATUS_VARNOTFOUND 3
#define CMDSTATUS_NOTFOUND    4
typedef struct col  { char coltitle[TELNET_CMD_MAXSIZE]; unsigned int coltype; } acol_t;
typedef struct line { char *val[TELNET_MAXCOL_NUM]; } aline_t;
typedef struct webdatadef {
  char  tblname[TELNET_HELPSTR_SIZE];
  int   numlines, numcols;
  acol_t columns[TELNET_MAXCOL_NUM];
  aline_t lines[TELNET_MAXLINE_NUM];
} webdatadef_t;
typedef void (*telnet_printfunc_t)(const char *format, ...);
typedef int  (*cmdfunc_t)(char *, int, telnet_printfunc_t prnt);
typedef int  (*webfunc_t)(char *cmdbuff, int debug, telnet_printfunc_t prnt, ...);
typedef int  (*webfunc_getdata_t)(const char *cmdbuff, int debug, void *data, telnet_printfunc_t prnt);
typedef int  (*qcmdfunc_t)(char *, int, telnet_printfunc_t prnt, void *arg);
#define TELNETSRV_CMDFLAG_PUSHINTPOOLQ        (1 << 0)
#define TELNETSRV_CMDFLAG_GETWEBDATA          (1 << 1)
#define TELNETSRV_CMDFLAG_TELNETONLY          (1 << 2)
#define TELNETSRV_CMDFLAG_WEBSRVONLY          (1 << 3)
#define TELNETSRV_CMDFLAG_CONFEXEC            (1 << 4)
#define TELNETSRV_CMDFLAG_GETWEBTBLDATA       (1 << 8)
#define TELNETSRV_CMDFLAG_NEEDPARAM           (1 << 10)
#define TELNETSRV_CMDFLAG_WEBSRV_SETRETURNTBL (1 << 11)
#define TELNETSRV_CMDFLAG_AUTOUPDATE          (1 << 12)
typedef struct cmddef {
  char cmdname[TELNET_CMD_MAXSIZE];
  char helpstr[TELNET_HELPSTR_SIZE];
  cmdfunc_t cmdfunc;
  union { webfunc_t webfunc; webfunc_getdata_t webfunc_getdata; };
  unsigned int cmdflags;
  void *qptr;
} telnetshell_cmddef_t;
#define TELNET_VARTYPE_INT32 1
typedef struct variabledef {
  char varname[TELNET_CMD_MAXSIZE];
  char vartype, checkval;
  void *varvalptr;
} telnetshell_vardef_t;
#define TELNET_ADDCMD_FNAME   "add_telnetcmd"
#define TELNET_POLLCMDQ_FNAME "poll_telnetcmdq"
#define TELNET_PUSHCMD_FNAME  "push_telnetcmd"
typedef int  (*add_telnetcmd_func_t)(const char *, telnetshell_vardef_t *, telnetshell_cmddef_t *);
typedef void (*poll_telnetcmdq_func_t)(void *qid, void *arg);
typedef void (*push_telnetcmd_func_t)(telnetshell_cmddef_t *cmd, char *cmdbuff, telnet_printfunc_t prnt);
