/*
 * BFDD - bfd_main.c   
 *
 * Copyright (C) 2007   Jaroslaw Adam Gralak
 *
 * This program is free software; you can redistribute it and/or modify it 
 * under the terms of the GNU General Public Licenseas published by the Free 
 * Software Foundation; either version 2 of the License, or (at your option) 
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,but WITHOUT 
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or 
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for 
 * more details.

 * You should have received a copy of the GNU General Public License along 
 * with this program; if not, write to the Free Software Foundation, Inc., 
 * 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */


#include <zebra.h>

#include <lib/version.h>
#include "getopt.h"
#include "thread.h"
#include "if.h"
#include "log.h"
#include "command.h"
#include "memory.h"
#include "sigevent.h"
#include "privs.h"

#include "bfdd/bfdd.h"
#include "bfdd/bfd_zebra.h"
#include "bfdd/bfd_interface.h"

/* bfdd privileges */
zebra_capabilities_t _caps_p[] = {
  ZCAP_NET_RAW,
  ZCAP_BIND,
};

struct zebra_privs_t bfdd_privs = {
#if defined(QUAGGA_USER) && defined(QUAGGA_GROUP)
  .user = QUAGGA_USER,
  .group = QUAGGA_GROUP,
#endif
#if defined(VTY_GROUP)
  .vty_group = VTY_GROUP,
#endif
  .caps_p = _caps_p,
  .cap_num_p = sizeof (_caps_p) / sizeof (_caps_p[0]),
  .cap_num_i = 0
};

/* Configuration filename and directory. */
char config_default[] = SYSCONFDIR BFDD_DEFAULT_CONFIG;

/* Manually specified configuration file name.  */
char *config_file = NULL;

/* Process ID saved for use by init system */
const char *pid_file = PATH_BFDD_PID;

/* Help information display. */
static void
usage (char *progname, int status)
{
  if (status != 0)
    fprintf (stderr, "Try `%s --help' for more information.\n", progname);
  else
    {
      printf ("Usage : %s [OPTION...]\n\
BFD Deamon\n\n\
-d, --daemon       Runs in daemon mode\n\
-f, --config_file  Set configuration file name\n\
-i, --pid_file     Set process identifier file name\n\
-A, --vty_addr     Set vty's bind address\n\
-P, --vty_port     Set vty's port number\n\
-C, --dryrun       Check configuration for validity and exit\n\
-u, --user         User to run as\n\
-g, --group        Group to run as\n\
-v, --version      Print program version\n\
-h, --help         Display this help and exit\n\
\n\
Report bugs to %s\n", progname, ZEBRA_BUG_ADDRESS);
    }

  exit (status);
}


/* VTY port number and address.  */
int vty_port = BFDD_VTY_PORT;
char *vty_addr = NULL;


/* BFDd Options */
static struct option longopts[] = {
  {"daemon", no_argument, NULL, 'd'},
  {"config_file", required_argument, NULL, 'f'},
  {"pid_file", required_argument, NULL, 'i'},
  {"help", no_argument, NULL, 'h'},
  {"dryrun", no_argument, NULL, 'C'},
  {"vty_addr", required_argument, NULL, 'A'},
  {"vty_port", required_argument, NULL, 'P'},
  {"retain", no_argument, NULL, 'r'},
  {"user", required_argument, NULL, 'u'},
  {"group", required_argument, NULL, 'g'},
  {"version", no_argument, NULL, 'v'},
  {0}
};

/* Master of threads. */
struct thread_master *master;

/* SIGHUP handler. */
static void
sighup (void)
{
  zlog (NULL, LOG_INFO, "SIGHUP received");
}

/* SIGINT / SIGTERM handler. */
static void
sigint (void)
{
  zlog_notice ("Terminating on signal");
  exit (0);
}

/* SIGUSR1 handler. */
static void
sigusr1 (void)
{
  zlog_rotate (NULL);
}


struct quagga_signal_t bfdd_signals[] = {
  {
   .signal = SIGHUP,
   .handler = &sighup,
   },
  {
   .signal = SIGUSR1,
   .handler = &sigusr1,
   },
  {
   .signal = SIGINT,
   .handler = &sigint,
   },
  {
   .signal = SIGTERM,
   .handler = &sigint,
   },
};


int
main (int argc, char **argv, char **envp)
{
  char *p;
  int daemon_mode = 0;
  int dryrun = 0;
  int vty_port = BFDD_VTY_PORT;
  char *progname;
  struct thread thread;

  umask (0027);


  progname = ((p = strrchr (argv[0], '/')) ? ++p : argv[0]);

  zlog_default = openzlog (progname, ZLOG_BFD,
			   LOG_CONS | LOG_NDELAY | LOG_PID, LOG_DAEMON);

  /* Command line option parse. */
  while (1)
    {
      int opt;

      opt = getopt_long (argc, argv, "df:i:hA:P:u:g:vC", longopts, 0);

      if (opt == EOF)
	break;

      switch (opt)
	{
	case 0:
	  break;
	case 'd':
	  daemon_mode = 1;
	  break;
	case 'f':
	  config_file = optarg;
	  break;
	case 'A':
	  vty_addr = optarg;
	  break;
	case 'i':
	  pid_file = optarg;
	  break;
	case 'P':
	  /* Deal with atoi() returning 0 on failure, and bfdd not
	     listening on bfd port... */
	  if (strcmp (optarg, "0") == 0)
	    {
	      vty_port = 0;
	      break;
	    }
	  vty_port = atoi (optarg);
	  vty_port = (vty_port ? vty_port : BFDD_VTY_PORT);
	  break;
	case 'C':
	  dryrun = 1;
	  break;
	case 'u':
	  bfdd_privs.user = optarg;
	  break;
	case 'g':
	  bfdd_privs.group = optarg;
	  break;
	case 'v':
	  print_version (progname);
	  exit (0);
	  break;
	case 'h':
	  usage (progname, 0);
	  break;
	default:
	  usage (progname, 1);
	  break;
	}
    }


  /* Prepare master thread. */
  master = thread_master_create ();

  /* Library initialization. */
  zprivs_init (&bfdd_privs);
  signal_init (master, array_size(bfdd_signals), bfdd_signals);
  cmd_init (1);
  vty_init (master);
  memory_init ();
  vrf_init ();

  /* random seed from time */
  srand (time (NULL));

  /* BFD related initialization. */
  bfd_init ();
  bfd_if_init ();
  bfd_zebra_init (master);
  bfd_vty_init ();  

  /* Get configuration file. */
  vty_read_config (config_file, config_default);

  //bfd_cfg();

  /* Start execution only if not in dry-run mode */
  if (dryrun)
    return (0);

  /* Change to the daemon program. */
  if (daemon_mode && daemon (0, 0) < 0)
    {
      zlog_err("BFDd daemon failed: %s", strerror(errno));
      return (1);
    }  

  /* Pid file create. */
  pid_output (pid_file);

  /* Create VTY's socket */
  vty_serv_sock (vty_addr, vty_port, BFD_VTYSH_PATH);

  /* Print banner. */
  zlog_notice ("BFDd %s starting: vty@%d", QUAGGA_VERSION, vty_port);

  /* Execute each thread. */
  while (thread_fetch (master, &thread))
    thread_call (&thread);

  /* Not reached. */
  return (0);
}
