#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/param.h>
#include <ctype.h>

#include "defs.h"
#include "provenance.h"
#include "../leveldb-1.14.0/include/leveldb/c.h"

/* macros */
#ifndef MAX
#define MAX(a,b)		(((a) > (b)) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a,b)		(((a) < (b)) ? (a) : (b))
#endif

extern void vbprintf(const char *fmt, ...);
void rstrip(char *s);

extern char CDE_exec_mode;
extern char CDE_verbose_mode;
extern char cde_pseudo_pkg_dir[MAXPATHLEN];

char CDE_provenance_mode = 0;
char CDE_bare_run = 0;
FILE* CDE_provenance_logfile = NULL;
pthread_mutex_t mut_logfile = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mut_pidlist = PTHREAD_MUTEX_INITIALIZER;
typedef struct {
  pid_t pv[1000]; // the list
  int pc; // total count
} pidlist_t;
static pidlist_t pidlist;

// leveldb
leveldb_t *db;
leveldb_options_t *options;

enum provenance_type {
  PRV_RDONLY=1, PRV_WRONLY=2, PRV_RDWR = 3, PRV_UNKNOWNIO=4, 
  PRV_SPAWN=17, PRV_LEXIT=18,
  STAT_MEM=33,
  PRV_INVALID=127};

extern int string_quote(const char *instr, char *outstr, int len, int size);
extern char* strcpy_from_child_or_null(struct tcb* tcp, long addr);
extern char* canonicalize_path(char* path, char* relpath_base);

void db_write_prov(long pid, int prv, const char *filename_abspath);
void add_pid_prov(pid_t pid);

/*
 * Print string specified by address `addr' and length `len'.
 * If `len' < 0, treat the string as a NUL-terminated string.
 * If string length exceeds `max_strlen', append `...' to the output.
 */
int
get_str_prov(char* dest, struct tcb *tcp, long addr, int len)
{
	static char *str = NULL;
	static char *outstr;
	int size;
  int len_written = 0;

	if (!addr) {
		len_written += sprintf(dest+len_written, "NULL");
		return len_written;
	}
	/* Allocate static buffers if they are not allocated yet. */
	if (!str)
		str = malloc(max_strlen + 1);
	if (!outstr)
		outstr = malloc(4 * max_strlen + sizeof "\"...\"");
	if (!str || !outstr) {
		fprintf(stderr, "out of memory\n");
		len_written += sprintf(dest+len_written, "%#lx", addr);
		return len_written;
	}

	if (len < 0) {
		/*
		 * Treat as a NUL-terminated string: fetch one byte more
		 * because string_quote() quotes one byte less.
		 */
		size = max_strlen + 1;
		str[max_strlen] = '\0';
		if (umovestr(tcp, addr, size, str) < 0) {
			len_written += sprintf(dest+len_written, "%#lx", addr);
			return len_written;
		}
	}
	else {
		size = MIN(len, max_strlen);
		if (umoven(tcp, addr, size, str) < 0) {
			len_written += sprintf(dest+len_written, "%#lx", addr);
			return len_written;
		}
	}

	if (string_quote(str, outstr, len, size) &&
	    (len < 0 || len > max_strlen))
		strcat(outstr, "...");

	len_written += sprintf(dest+len_written, "%s", outstr);
  return len_written;
}

void
print_arg_prov(char* argstr, struct tcb *tcp, long addr)
{
	union {
		unsigned int p32;
		unsigned long p64;
		char data[sizeof(long)];
	} cp;
	const char *sep;
	int n = 0;
  unsigned int len = 0;

  len += sprintf(argstr+len, "[");
	cp.p64 = 1;
	for (sep = ""; !abbrev(tcp) || n < max_strlen / 2; sep = ", ", ++n) {
		if (umoven(tcp, addr, personality_wordsize[current_personality],
			   cp.data) < 0) {
			len += sprintf(argstr+len, "%#lx\n", addr);
			return;
		}
		if (personality_wordsize[current_personality] == 4)
			cp.p64 = cp.p32;
		if (cp.p64 == 0)
			break;
		len += sprintf(argstr+len, "%s", sep);
		len += get_str_prov(argstr+len, tcp, cp.p64, -1);
		addr += personality_wordsize[current_personality];
	}
	if (cp.p64)
		len += sprintf(argstr+len, "%s...", sep);

	len += sprintf(argstr+len, "]");
}


void print_exec_prov(struct tcb *tcp) {
  if (CDE_provenance_mode) {
    char* opened_filename = strcpy_from_child_or_null(tcp, tcp->u_arg[0]);
    char* filename_abspath = canonicalize_path(opened_filename, tcp->current_dir);
    int parentPid = tcp->parent == NULL ? -1 : tcp->parent->pid;
    char args[MAXPATHLEN];
    if (parentPid==-1) parentPid = getpid();
    assert(filename_abspath);
    print_arg_prov(args, tcp, tcp->u_arg[1]);
    fprintf(CDE_provenance_logfile, "%d %d EXECVE %u %s %s %s\n", (int)time(0), 
      parentPid, tcp->pid, filename_abspath, tcp->current_dir, args);
    db_write_prov_exec(parentPid, tcp->pid, filename_abspath, tcp->current_dir, args);
    if (CDE_verbose_mode) {
      vbprintf("[%d-prov] BEGIN %s '%s'\n", tcp->pid, "execve", opened_filename);
    }
    free(filename_abspath);
    free(opened_filename);
  }
}

void print_execdone_prov(struct tcb *tcp) {
  if (CDE_provenance_mode) {
    int ppid = -1;
    if (tcp->parent) ppid = tcp->parent->pid;
    fprintf(CDE_provenance_logfile, "%d %u EXECVE2 %d\n", (int)time(0), tcp->pid, ppid);
    db_write_prov_execdone(tcp->pid, ppid);
    add_pid_prov(tcp->pid);
    if (CDE_verbose_mode) {
      vbprintf("[%d-prov] BEGIN %s '%s'\n", tcp->pid, "execve2");
    }
  }
}

void print_IO_prov(struct tcb *tcp, char* filename, const char* syscall_name) {
  if (CDE_provenance_mode) {
    // only track open syscalls
    if ((tcp->u_rval >= 0) &&
        strcmp(syscall_name, "sys_open") == 0) {
      char* filename_abspath = canonicalize_path(filename, tcp->current_dir);
      assert(filename_abspath);

      // Note: tcp->u_arg[1] is only for open(), not openat()
      unsigned char open_mode = (tcp->u_arg[1] & 3);
      if (open_mode == O_RDONLY) {
        fprintf(CDE_provenance_logfile, "%d %u READ %s\n", (int)time(0), tcp->pid, filename_abspath);
        db_write_prov(tcp->pid, PRV_RDONLY, filename_abspath);
      }
      else if (open_mode == O_WRONLY) {
        fprintf(CDE_provenance_logfile, "%d %u WRITE %s\n", (int)time(0), tcp->pid, filename_abspath);
        db_write_prov(tcp->pid, PRV_WRONLY, filename_abspath);
      }
      else if (open_mode == O_RDWR) {
        fprintf(CDE_provenance_logfile, "%d %u READ-WRITE %s\n", (int)time(0), tcp->pid, filename_abspath);
        db_write_prov(tcp->pid, PRV_RDWR, filename_abspath);
      }
      else {
        fprintf(CDE_provenance_logfile, "%d %u UNKNOWNIO %s\n", (int)time(0), tcp->pid, filename_abspath);
        db_write_prov(tcp->pid, PRV_UNKNOWNIO, filename_abspath);
      }

      free(filename_abspath);
    }
  }
}

void print_spawn_prov(struct tcb *tcp) {
  if (CDE_provenance_mode) {
    fprintf(CDE_provenance_logfile, "%d %u SPAWN %u\n", (int)time(0), tcp->parent->pid, tcp->pid);
    db_write_prov_int(tcp->parent->pid, PRV_SPAWN, tcp->pid);
  }
}

/*
void print_act_prov(struct tcb *tcp, const char* action) {
  if (CDE_provenance_mode) {
    fprintf(CDE_provenance_logfile, "%d %u %s 0\n", (int)time(0), tcp->pid, action);
    db_write_prov(tcp->pid, PRV_ACTION, action);
  }
}*/

void print_sock_prov(struct tcb *tcp, const char *op, unsigned int port, unsigned long ipv4) {
  print_newsock_prov(tcp, op, 0, 0, port, ipv4, 0);
}
void print_newsock_prov(struct tcb *tcp, const char* op, \
  unsigned int s_port, unsigned long s_ipv4, \
  unsigned int d_port, unsigned long d_ipv4, int sk) {
  struct in_addr s_in, d_in;
  char saddr[32], daddr[32];
  s_in.s_addr = s_ipv4;
  strcpy(saddr, inet_ntoa(s_in));
  d_in.s_addr = d_ipv4;
  strcpy(daddr, inet_ntoa(d_in));
  if (CDE_provenance_mode) {
    fprintf(CDE_provenance_logfile, "%d %u %s %u %s %u %s %d\n", (int)time(0), tcp->pid, \
        op, s_port, saddr, d_port, daddr, sk);
    // TODO: db_write_prov(tcp->pid, PRV_WRONLY, filename_abspath);
  }
}

void print_curr_prov(pidlist_t *pidlist_p) {
  int i, curr_time;
  FILE *f;
  char buff[1024];
  long unsigned int rss;

  pthread_mutex_lock(&mut_pidlist);
  curr_time = (int)time(0);
  for (i = 0; i < pidlist_p->pc; i++) {
    sprintf(buff, "/proc/%d/stat", pidlist_p->pv[i]);
    f = fopen(buff, "r");
    if (f==NULL) { // remove this invalid pid
      fprintf(CDE_provenance_logfile, "%d %u LEXIT\n", curr_time, pidlist_p->pv[i]); // lost_pid exit
      db_write_prov(pidlist_p->pv[i], PRV_LEXIT, "");
      pidlist_p->pv[i] = pidlist_p->pv[pidlist_p->pc-1];
      pidlist_p->pc--;
      continue;
    }
    if (fgets(buff, 1024, f) == NULL)
      rss= 0;
    else
      // details of format: http://git.kernel.org/?p=linux/kernel/git/stable/linux-stable.git;a=blob_plain;f=fs/proc/array.c;hb=d1c3ed669a2d452cacfb48c2d171a1f364dae2ed
      sscanf(buff, "%*d %*s %*c %*d %*d %*d %*d %*d %*lu %*lu \
%*lu %*lu %*lu %*lu %*lu %*ld %*ld %*ld %*ld %*ld %*ld %*lu %lu ", &rss);
    fclose(f);
    fprintf(CDE_provenance_logfile, "%d %u MEM %lu\n", curr_time, pidlist_p->pv[i], rss);
    db_write_prov_int(pidlist_p->pv[i], STAT_MEM, rss);
  }
  pthread_mutex_unlock(&mut_pidlist);
}

void *capture_cont_prov(void* ptr) {
  pidlist_t *pidlist_p = (pidlist_t*) ptr;
  // Wait till we have the first pid, which should be the traced process.
  // Start recording memory footprint.
  // Ok to stop when there is no more pid, since by then,
  // the original tracded process should have stopped.
  while (pidlist_p->pc == 0) usleep(100000);
  while (pidlist_p->pc > 0) { // recording
    print_curr_prov(pidlist_p);
    sleep(1); // TODO: configurable
  } // done recording: pidlist.pc == 0
  pthread_mutex_destroy(&mut_pidlist);

	if (CDE_provenance_logfile) {
  	fclose(CDE_provenance_logfile);
    leveldb_close(db);
  }
  pthread_mutex_destroy(&mut_logfile);

  return NULL;
}

void db_write(const char* key, const char* value) {
  leveldb_writeoptions_t *woptions;
  char *err = NULL;
  assert(db!=NULL);

  woptions = leveldb_writeoptions_create();
  if (value == NULL)
    leveldb_put(db, woptions, key, strlen(key), value, 0, &err);
  else
    leveldb_put(db, woptions, key, strlen(key), value, strlen(value), &err);

  if (err != NULL) {
    vbprintf("DB - Write FAILED: '%s' -> '%s'\n", key, value);
  }

  leveldb_free(err); err = NULL;
}

void db_write_int(const char* key, int value) {
  char val[16];
  sprintf(val, "%d", value);
  db_write(key, val);
}

char* db_readc(char* key) {
  leveldb_readoptions_t *roptions;
  char *err = NULL;
  char *read, *ret;
  size_t read_len;

  roptions = leveldb_readoptions_create();
  read = leveldb_get(db, roptions, key, strlen(key), &read_len, &err);

  if (err != NULL) {
    vbprintf("DB - Read FAILED: '%s'\n", key);
    return NULL;
  }
  ret = malloc(read_len+1);
  strncpy(ret, read, read_len);
  ret[read_len] = 0;
  
  leveldb_free(err); err = NULL;
  
  return ret;
}

void init_prov() {
  pthread_t ptid;
  char* env_prov_mode = getenv("IN_CDE_PROVENANCE_MODE");
  char path[PATH_MAX];
  int subns=1;
  char *err = NULL;
  
  if (env_prov_mode != NULL)
    CDE_provenance_mode = (strcmp(env_prov_mode, "1") == 0) ? 1 : 0;
  else
    CDE_provenance_mode = !CDE_exec_mode;
  if (CDE_provenance_mode) {
    setenv("IN_CDE_PROVENANCE_MODE", "1", 1);
    pthread_mutex_init(&mut_logfile, NULL);
    // create NEW provenance log file
    bzero(path, sizeof(path));
    sprintf(path, "%s/provenance.%s.1.log", cde_pseudo_pkg_dir, CDE_ROOT_NAME);
    if (access(path, R_OK)==-1)
      CDE_provenance_logfile = fopen(path, "w");
    else {
      // check through provenance.$subns.log to find a new file name
      do {
        subns++;
        bzero(path, sizeof(path));
        sprintf(path, "%s/provenance.%s.%d.log", cde_pseudo_pkg_dir, CDE_ROOT_NAME, subns);
      } while (access(path, R_OK)==0);
      fprintf(stderr, "Provenance log file: %s\n", path);
      CDE_provenance_logfile = fopen(path, "w");
    }
    
    // leveldb initialization
    sprintf(path+strlen(path), "_db");
    fprintf(stderr, "Provenance db: %s\n", path);
    options = leveldb_options_create();
    leveldb_options_set_create_if_missing(options, 1);
    db = leveldb_open(options, path, &err);
    if (err != NULL || db == NULL) {
      fprintf(stderr, "leveldb open fail.\n");
      exit(-1);
    }
    assert(db!=NULL);
    /* reset error var */
    leveldb_free(err); err = NULL;

    char* username = getlogin();
    FILE *fp;
    char uname[PATH_MAX];
    fp = popen("uname -a", "r");
    if (fgets(uname, PATH_MAX, fp) == NULL)
      sprintf(uname, "(unknown architecture)");
    pclose(fp);
    rstrip(uname);
    char fullns[PATH_MAX];
    sprintf(fullns, "%s.%d", CDE_ROOT_NAME, subns);
    fprintf(CDE_provenance_logfile, "# @agent: %s\n", username == NULL ? "(noone)" : username);
    fprintf(CDE_provenance_logfile, "# @machine: %s\n", uname);
    fprintf(CDE_provenance_logfile, "# @namespace: %s\n", CDE_ROOT_NAME);
    fprintf(CDE_provenance_logfile, "# @subns: %d\n", subns);
    fprintf(CDE_provenance_logfile, "# @fullns: %s\n", fullns);
    fprintf(CDE_provenance_logfile, "# @parentns: %s\n", getenv("CDE_PROV_NAMESPACE"));
    
    // provenance meta data in lvdb
    db_write("meta.agent", username == NULL ? "(noone)" : username);
    db_write("meta.machine", uname);
    db_write("meta.namespace", CDE_ROOT_NAME);
    db_write_int("meta.subns", subns);
    db_write("meta.fullns", fullns);
    db_write("meta.parentns", getenv("CDE_PROV_NAMESPACE"));
    
    setenv("CDE_PROV_NAMESPACE", fullns, 1);

    pthread_mutex_init(&mut_pidlist, NULL);
    pidlist.pc = 0;
    pthread_create( &ptid, NULL, capture_cont_prov, &pidlist);
  }
}

void add_pid_prov(pid_t pid) {
  pthread_mutex_lock(&mut_pidlist);
  pidlist.pv[pidlist.pc] = pid;
  pidlist.pc++;
  pthread_mutex_unlock(&mut_pidlist);
  print_curr_prov(&pidlist);
}

void rm_pid_prov(pid_t pid) {
  int i=0;
  assert(pidlist.pc>0);
  print_curr_prov(&pidlist);
  pthread_mutex_lock(&mut_pidlist);
  while (pidlist.pv[i] != pid && i < pidlist.pc) i++;
  if (i < pidlist.pc) {
    pidlist.pv[i] = pidlist.pv[pidlist.pc-1];
    pidlist.pc--;
  }
  pthread_mutex_unlock(&mut_pidlist);
}

void rstrip(char *s) {
  size_t size;
  char *end;

  size = strlen(s);

  if (!size)
    return;

  end = s + size - 1;
  while (end >= s && isspace(*end))
    end--;
  *(end + 1) = '\0';

}


