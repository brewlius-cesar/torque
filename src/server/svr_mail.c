/*
*         OpenPBS (Portable Batch System) v2.3 Software License
*
* Copyright (c) 1999-2000 Veridian Information Solutions, Inc.
* All rights reserved.
*
* ---------------------------------------------------------------------------
* For a license to use or redistribute the OpenPBS software under conditions
* other than those described below, or to purchase support for this software,
* please contact Veridian Systems, PBS Products Department ("Licensor") at:
*
*    www.OpenPBS.org  +1 650 967-4675                  sales@OpenPBS.org
*                        877 902-4PBS (US toll-free)
* ---------------------------------------------------------------------------
*
* This license covers use of the OpenPBS v2.3 software (the "Software") at
* your site or location, and, for certain users, redistribution of the
* Software to other sites and locations.  Use and redistribution of
* OpenPBS v2.3 in source and binary forms, with or without modification,
* are permitted provided that all of the following conditions are met.
* After December 31, 2001, only conditions 3-6 must be met:
*
* 1. Commercial and/or non-commercial use of the Software is permitted
*    provided a current software registration is on file at www.OpenPBS.org.
*    If use of this software contributes to a publication, product, or
*    service, proper attribution must be given; see www.OpenPBS.org/credit.html
*
* 2. Redistribution in any form is only permitted for non-commercial,
*    non-profit purposes.  There can be no charge for the Software or any
*    software incorporating the Software.  Further, there can be no
*    expectation of revenue generated as a consequence of redistributing
*    the Software.
*
* 3. Any Redistribution of source code must retain the above copyright notice
*    and the acknowledgment contained in paragraph 6, this list of conditions
*    and the disclaimer contained in paragraph 7.
*
* 4. Any Redistribution in binary form must reproduce the above copyright
*    notice and the acknowledgment contained in paragraph 6, this list of
*    conditions and the disclaimer contained in paragraph 7 in the
*    documentation and/or other materials provided with the distribution.
*
* 5. Redistributions in any form must be accompanied by information on how to
*    obtain complete source code for the OpenPBS software and any
*    modifications and/or additions to the OpenPBS software.  The source code
*    must either be included in the distribution or be available for no more
*    than the cost of distribution plus a nominal fee, and all modifications
*    and additions to the Software must be freely redistributable by any party
*    (including Licensor) without restriction.
*
* 6. All advertising materials mentioning features or use of the Software must
*    display the following acknowledgment:
*
*     "This product includes software developed by NASA Ames Research Center,
*     Lawrence Livermore National Laboratory, and Veridian Information
*     Solutions, Inc.
*     Visit www.OpenPBS.org for OpenPBS software support,
*     products, and information."
*
* 7. DISCLAIMER OF WARRANTY
*
* THIS SOFTWARE IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND. ANY EXPRESS
* OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
* OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND NON-INFRINGEMENT
* ARE EXPRESSLY DISCLAIMED.
*
* IN NO EVENT SHALL VERIDIAN CORPORATION, ITS AFFILIATED COMPANIES, OR THE
* U.S. GOVERNMENT OR ANY OF ITS AGENCIES BE LIABLE FOR ANY DIRECT OR INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
* LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
* OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
* LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
* NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
* EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
* This license will be governed by the laws of the Commonwealth of Virginia,
* without reference to its choice of law rules.
*/
/*
 * svr_mail.c - send mail to mail list or owner of job on
 * job begin, job end, and/or job abort
 */

#include <pbs_config.h>   /* the master config generated by configure */

#include "pbs_ifl.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "list_link.h"
#include "attribute.h"
#include "server_limits.h"
#include "pbs_job.h"
#include "log.h"
#include "../lib/Liblog/pbs_log.h"
#include "../lib/Liblog/log_event.h"
#include "server.h"
#include "utils.h"
#include "threadpool.h"
#include "svrfunc.h" /* get_svr_attr_* */
#include "work_task.h"
#include <sys/wait.h>

/* Unit tests should use the special unit test sendmail command */
#ifdef UT_SENDMAIL_CMD
#undef SENDMAIL_CMD
#define SENDMAIL_CMD UT_SENDMAIL_CMD
#endif

/* External Functions Called */

extern void net_close_without_mutexes ();
extern void svr_format_job (FILE *, mail_info *, const char *);
extern int  listening_socket;

/* Global Data */
extern struct server server;

extern int LOGLEVEL;



void free_mail_info(

  mail_info *mi)

  {
  if (mi->exec_host)
    free(mi->exec_host);

  if (mi->jobname)
    free(mi->jobname);

  if (mi->text)
    free(mi->text);

  if (mi->errFile)
    free(mi->errFile);

  if (mi->outFile)
    free(mi->outFile);

  free(mi->jobid);
  free(mi->mailto);

  free(mi);
  } /* END free_mail_info() */



void add_body_info(

  char      *bodyfmtbuf /* I */,
  mail_info *mi /* I */)
  {
  char *bodyfmt = NULL;
  bodyfmt =  strcpy(bodyfmtbuf, "PBS Job Id: %i\n"
                                  "Job Name:   %j\n");
  if (mi->exec_host != NULL)
    {
    strcat(bodyfmt, "Exec host:  %h\n");
    }

  strcat(bodyfmt, "%m\n");

  if (mi->text != NULL)
    {
    strcat(bodyfmt, "%d\n");
    }

  if (mi->errFile != NULL)
    {
    strcat(bodyfmt, "Error_Path: %k\n");
    }

  if (mi->outFile !=NULL)
    {
    strcat(bodyfmt, "Output_Path: %l\n");
    }
  }


/*
 * write_email()
 *
 * In emailing, the mail body is written to a pipe connected to
 * standard input for sendmail. This function supplies the body
 * of the message.
 *
 */
void write_email(

  FILE      *outmail_input,
  mail_info *mi)

  {
  char       *bodyfmt = NULL;
  const char *subjectfmt = NULL;

  /* Pipe in mail headers: To: and Subject: */
  fprintf(outmail_input, "To: %s\n", mi->mailto);

  /* mail subject line formating statement */
  get_svr_attr_str(SRV_ATR_MailSubjectFmt, (char **)&subjectfmt);
  if (subjectfmt == NULL)
    {
    subjectfmt = "PBS JOB %i";
    }

  fprintf(outmail_input, "Subject: ");
  svr_format_job(outmail_input, mi, subjectfmt);
  fprintf(outmail_input, "\n");

  /* Set "Precedence: bulk" to avoid vacation messages, etc */
  fprintf(outmail_input, "Precedence: bulk\n\n");

  /* mail body formating statement */
  get_svr_attr_str(SRV_ATR_MailBodyFmt, &bodyfmt);
  if (bodyfmt == NULL)
    {
    char bodyfmtbuf[MAXLINE];
    add_body_info(bodyfmtbuf, mi);
    bodyfmt = bodyfmtbuf;
    }

  /* Now pipe in the email body */
  svr_format_job(outmail_input, mi, bodyfmt);

  } /* write_email() */


/*
 * send_the_mail()
 *
 * In emailing, we fork and exec sendmail providing the body of
 * the message on standard in.
 *
 */
void *send_the_mail(

  void *vp)

  {
  mail_info  *mi = (mail_info *)vp;

  int         status = 0;
  int         numargs = 0;
  int         pipes[2];
  int         counter;
  pid_t       pid;
  char       *mailptr;
  const char *mailfrom = NULL;
  char        tmpBuf[LOG_BUF_SIZE];
  // We call sendmail with cmd_name + 2 arguments + # of mailto addresses + 1 for null
  char       *sendmail_args[100];
  FILE       *stream;


  /* Who is mail from, if SRV_ATR_mailfrom not set use default */
  get_svr_attr_str(SRV_ATR_mailfrom, (char **)&mailfrom);
  if (mailfrom == NULL)
    {
    mailfrom = PBS_DEFAULT_MAIL;
    if (LOGLEVEL >= 5)
      {
      char tmpBuf[LOG_BUF_SIZE];

      snprintf(tmpBuf,sizeof(tmpBuf),
        "Updated mailfrom to default: '%s'\n",
        mailfrom);
      log_event(PBSEVENT_ERROR | PBSEVENT_ADMIN | PBSEVENT_JOB,
        PBS_EVENTCLASS_JOB,
        mi->jobid,
        tmpBuf);
      }
    }

  sendmail_args[numargs++] = (char *)SENDMAIL_CMD;
  sendmail_args[numargs++] = (char *)"-f";
  sendmail_args[numargs++] = (char *)mailfrom;

  /* Add the e-mail addresses to the command line */
  mailptr = strdup(mi->mailto);
  sendmail_args[numargs++] = mailptr;
  for (counter=0; counter < (int)strlen(mailptr); counter++)
    {
    if (mailptr[counter] == ',')
      {
      mailptr[counter] = '\0';
      sendmail_args[numargs++] = mailptr + counter + 1;
      if (numargs >= 99)
        break;
      }
    }

  sendmail_args[numargs] = NULL;

  /* Create a pipe to talk to the sendmail process we are about to fork */
  if (pipe(pipes) == -1)
    {
    snprintf(tmpBuf, sizeof(tmpBuf), "Unable to pipes for sending e-mail\n");
    log_event(PBSEVENT_ERROR | PBSEVENT_ADMIN | PBSEVENT_JOB,
      PBS_EVENTCLASS_JOB,
      mi->jobid,
      tmpBuf);

    free_mail_info(mi);
    free(mailptr);
    return(NULL);
    }

  if ((pid=fork()) == -1)
    {
    snprintf(tmpBuf, sizeof(tmpBuf), "Unable to fork for sending e-mail\n");
    log_event(PBSEVENT_ERROR | PBSEVENT_ADMIN | PBSEVENT_JOB,
      PBS_EVENTCLASS_JOB,
      mi->jobid,
      tmpBuf);

    free_mail_info(mi);
    free(mailptr);
    close(pipes[0]);
    close(pipes[1]);
    return(NULL);
    }
  else if (pid == 0)
    {
    /* CHILD */

    /* close all open network sockets */
    net_close_without_mutexes();

    // this socket isn't in the connections table so it doesn't get closed by net_close_without_mutexes().
    // the connections table has it marked as idle so it doesn't call close()
    close(listening_socket);

    /* Close the write end of the pipe, make read end stdin */
    dup2(pipes[0],STDIN_FILENO);
    close(pipes[0]);
    close(pipes[1]);
    execv(SENDMAIL_CMD, sendmail_args);
    /* This never returns, but if the execv fails the child should exit */
    exit(1);
    }
  else
    {
    /* This is the parent */

    /* Close the read end of the pipe */
    close(pipes[0]);

    /* Write the body to the pipe */
    stream = fdopen(pipes[1], "w");
    write_email(stream, mi);

    /* Close and wait for the command to finish */
    if (fclose(stream) != 0)
      {
      snprintf(tmpBuf,sizeof(tmpBuf),
        "Piping mail body to sendmail closed: errno %d:%s\n",
        errno, strerror(errno));

      log_event(PBSEVENT_ERROR | PBSEVENT_ADMIN | PBSEVENT_JOB,
        PBS_EVENTCLASS_JOB,
        mi->jobid,
        tmpBuf);
      }
    waitpid(pid, &status, 0);

    if (status != 0)
      {
      snprintf(tmpBuf,sizeof(tmpBuf),
        "Sendmail command returned %d. Mail may not have been sent\n",
        status);

      log_event(PBSEVENT_ERROR | PBSEVENT_ADMIN | PBSEVENT_JOB,
        PBS_EVENTCLASS_JOB,
        mi->jobid,
        tmpBuf);
      }
      
    free_mail_info(mi);
    free(mailptr);
    return(NULL);
    }
    
  /* NOT REACHED */

  return(NULL);
  } /* END send_the_mail() */



int add_fileinfo(

  const char           *attrVal,                  /* I */      
  char                **filename,                 /* O */
  mail_info            *mi,                       /* I/O */
  const pbs_attribute   job_attr[JOB_ATR_LAST], /* I */
  const char           *memory_err)               /* I */

  {
  char *attributeValue = (char *)attrVal;
 
  if (job_attr[JOB_ATR_join].at_flags & ATR_VFLAG_SET)
    {
    if (!(strncmp(job_attr[JOB_ATR_join].at_val.at_str, "oe", 2)))
      attributeValue = job_attr[JOB_ATR_outpath].at_val.at_str;
    else if (!(strncmp(job_attr[JOB_ATR_join].at_val.at_str, "eo", 2)))
      attributeValue = job_attr[JOB_ATR_errpath].at_val.at_str;
    }
  if (attributeValue != NULL)
    {
    *filename = strdup(attributeValue);

    if (*filename == NULL)
      {
      log_err(ENOMEM, __func__, memory_err);
      free(mi);
      return -1;
      }
    }
  else
    *filename = NULL;

  return 0;
  }

void svr_mailowner_with_message(

  job   *pjob,      /* I */
  int    mailpoint, /* note, single character  */
  int    force,     /* if set to MAIL_FORCE, force mail delivery */
  const char  *text, /* text to mail. */
  const char *msg)   /* Optional extra message */
  {
  if((text == NULL)||(*text == '\0'))
    {
    return;
    }
  if((msg == NULL)||(*msg == '\0'))
    {
    return svr_mailowner(pjob,mailpoint,force,text);
    }
  char *newMsg = (char *)malloc(strlen(text) + strlen(msg) + 2);
  if(newMsg == NULL)
    {
    return;
    }
  strcpy(newMsg,text);
  strcat(newMsg,"\n");
  strcat(newMsg,msg);
  svr_mailowner(pjob,mailpoint,force,newMsg);
  free(newMsg);
  }

void svr_mailowner(

  job   *pjob,      /* I */
  int    mailpoint, /* note, single character  */
  int    force,     /* if set to MAIL_FORCE, force mail delivery */
  const char  *text)      /* (optional) additional message text */

  {
  static const char   *memory_err = "Cannot allocate memory to send email";

  char                  mailto[1024];
  char                 *domain = NULL;
  int                   i;
  mail_info            *mi;
  long                  no_force = FALSE;

  struct array_strings *pas;
  memset(mailto, 0, sizeof(mailto));

  get_svr_attr_str(SRV_ATR_MailDomain, &domain);
  if ((domain != NULL) &&
      (!strcasecmp("never", domain)))
    {
    /* never send user mail under any conditions */
    if (LOGLEVEL >= 3) 
      {
      log_event(PBSEVENT_ERROR | PBSEVENT_ADMIN | PBSEVENT_JOB,
        PBS_EVENTCLASS_JOB,
        pjob->ji_qs.ji_jobid,
        "Not sending email: Mail domain set to 'never'\n");
      }

    return;
    }

  if (LOGLEVEL >= 3)
    {
    char tmpBuf[LOG_BUF_SIZE];

    snprintf(tmpBuf, LOG_BUF_SIZE, "preparing to send '%c' mail for job %s to %s (%.64s)\n",
             (char)mailpoint,
             pjob->ji_qs.ji_jobid,
             pjob->ji_wattr[JOB_ATR_job_owner].at_val.at_str,
             (text != NULL) ? text : "---");

    log_event(
      PBSEVENT_ERROR | PBSEVENT_ADMIN | PBSEVENT_JOB,
      PBS_EVENTCLASS_JOB,
      pjob->ji_qs.ji_jobid,
      tmpBuf);
    }

  /*
   * if force is true, force the mail out regardless of mailpoint
   * unless server no_mail_force attribute is set to true
   */
  get_svr_attr_l(SRV_ATR_NoMailForce, &no_force);

  if ((force != MAIL_FORCE) ||
      (no_force == TRUE))
    {

    if (pjob->ji_wattr[JOB_ATR_mailpnts].at_flags & ATR_VFLAG_SET)
      {
      if (*(pjob->ji_wattr[JOB_ATR_mailpnts].at_val.at_str) ==  MAIL_NONE)
        {
        /* do not send mail. No mail requested on job */
        log_event(PBSEVENT_JOB,
                  PBS_EVENTCLASS_JOB,
                  pjob->ji_qs.ji_jobid,
                  "Not sending email: job requested no e-mail");
        return;
        }
      /* see if user specified mail of this type */
      if (strchr(
            pjob->ji_wattr[JOB_ATR_mailpnts].at_val.at_str,
            mailpoint) == NULL)
        {
        /* do not send mail */
        log_event(PBSEVENT_ERROR | PBSEVENT_ADMIN | PBSEVENT_JOB,
          PBS_EVENTCLASS_JOB,
          pjob->ji_qs.ji_jobid,
          "Not sending email: User does not want mail of this type.\n");

        return;
        }
      }
    else if (mailpoint != MAIL_ABORT) /* not set, default to abort */
      {
      log_event(PBSEVENT_ERROR | PBSEVENT_ADMIN | PBSEVENT_JOB,
        PBS_EVENTCLASS_JOB,
        pjob->ji_qs.ji_jobid,
        "Not sending email: Default mailpoint does not include this type.\n");

      return;
      }
    }

  mi = (mail_info *)calloc(1, sizeof(mail_info));

  if (mi == NULL)
    {
    log_err(ENOMEM, __func__, memory_err);
    return;
    }

  /* Who does the mail go to?  If mail-list, them; else owner */
  mailto[0] = '\0';

  if (pjob->ji_wattr[JOB_ATR_mailuser].at_flags & ATR_VFLAG_SET)
    {
    /* has mail user list, send to them rather than owner */

    pas = pjob->ji_wattr[JOB_ATR_mailuser].at_val.at_arst;

    if (pas != NULL)
      {
      for (i = 0;i < pas->as_usedptr;i++)
        {
        if ((strlen(mailto) + strlen(pas->as_string[i]) + 2) < sizeof(mailto))
          {
          if (mailto[0] != '\0')
            strcat(mailto, ",");

          strcat(mailto, pas->as_string[i]);
          }
        }
      }
      mailto[strlen(mailto)] = '\0';
    }
  else
    {
    /* no mail user list, just send to owner */

    if (domain != NULL)
      {
      snprintf(mailto, sizeof(mailto), "%s@%s",
        pjob->ji_wattr[JOB_ATR_euser].at_val.at_str, domain);

      if (LOGLEVEL >= 5) 
        {
        char tmpBuf[LOG_BUF_SIZE];

        snprintf(tmpBuf,sizeof(tmpBuf),
          "Updated mailto from job owner and mail domain: '%s'\n",
          mailto);
        log_event(PBSEVENT_ERROR | PBSEVENT_ADMIN | PBSEVENT_JOB,
          PBS_EVENTCLASS_JOB,
          pjob->ji_qs.ji_jobid,
          tmpBuf);
        }
      }
    else
      {
#ifdef TMAILDOMAIN
      snprintf(mailto, sizeof(mailto), "%s@%s",
        pjob->ji_wattr[JOB_ATR_euser].at_val.at_str, TMAILDOMAIN);
#else /* TMAILDOMAIN */
      snprintf(mailto, sizeof(mailto), "%s", pjob->ji_wattr[JOB_ATR_job_owner].at_val.at_str);
#endif /* TMAILDOMAIN */

      if (LOGLEVEL >= 5)
        {
        char tmpBuf[LOG_BUF_SIZE];

        snprintf(tmpBuf,sizeof(tmpBuf),
          "Updated mailto from job owner: '%s'\n",
          mailto);
        log_event(PBSEVENT_ERROR | PBSEVENT_ADMIN | PBSEVENT_JOB,
          PBS_EVENTCLASS_JOB,
          pjob->ji_qs.ji_jobid,
          tmpBuf);
        }
      }
    }

  /* initialize the mail information */

  if ((mi->mailto = strdup(mailto)) == NULL)
    {
    log_err(ENOMEM, __func__, memory_err);
    free(mi);
    return;
    }

  mi->mail_point = mailpoint;

  if (pjob->ji_wattr[JOB_ATR_exec_host].at_val.at_str != NULL)
    {
    mi->exec_host = strdup(pjob->ji_wattr[JOB_ATR_exec_host].at_val.at_str);

    if (mi->exec_host == NULL)
      {
      log_err(ENOMEM, __func__, memory_err);
      free(mi);
      return;
      }
    }
  else
    mi->exec_host = NULL;

  if ((mi->jobid = strdup(pjob->ji_qs.ji_jobid)) == NULL)
    {
    log_err(ENOMEM, __func__, memory_err);
    free(mi);
    return;
    }

  if (pjob->ji_wattr[JOB_ATR_jobname].at_val.at_str != NULL)
    {
    mi->jobname = strdup(pjob->ji_wattr[JOB_ATR_jobname].at_val.at_str);

    if (mi->jobname == NULL)
      {
      log_err(ENOMEM, __func__, memory_err);
      free(mi);
      return;
      }
    }
  else
    mi->jobname = NULL;

  if (mailpoint == (int) MAIL_END)
    {
    if (add_fileinfo((const char*)pjob->ji_wattr[JOB_ATR_errpath].at_val.at_str,
        &(mi->errFile), mi, pjob->ji_wattr, memory_err))
      return;
    else if (add_fileinfo((char *)pjob->ji_wattr[JOB_ATR_outpath].at_val.at_str, 
             &(mi->outFile), mi, pjob->ji_wattr, memory_err))
      return;
   }

  if (text)
    {
    if ((mi->text = strdup(text)) == NULL)
      {
      free(mi);
      log_err(ENOMEM, __func__, memory_err);
      return;
      }
    }
  else
    mi->text = NULL;

  /* have a thread do the work of sending the mail */
  enqueue_threadpool_request(send_the_mail,mi);

  return;
  }  /* END svr_mailowner() */

/* END svr_mail.c */
