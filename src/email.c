/*
** Copyright (c) 2007 D. Richard Hipp
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the Simplified BSD License (also
** known as the "2-Clause License" or "FreeBSD License".)
**
** This program is distributed in the hope that it will be useful,
** but without any warranty; without even the implied warranty of
** merchantability or fitness for a particular purpose.
**
** Author contact information:
**   drh@hwaci.com
**   http://www.hwaci.com/drh/
**
*******************************************************************************
**
** Email notification features
*/
#include "config.h"
#include "email.h"
#include <assert.h>

/*
** Maximum size of the subscriberCode blob, in bytes
*/
#define SUBSCRIBER_CODE_SZ 32

/*
** SQL code to implement the tables needed by the email notification
** system.
*/
static const char zEmailInit[] =
@ -- Subscribers are distinct from users.  A person can have a log-in in
@ -- the USER table without being a subscriber.  Or a person can be a
@ -- subscriber without having a USER table entry.  Or they can have both.
@ -- In the last case the suname column points from the subscriber entry
@ -- to the USER entry.
@ --
@ -- The ssub field is a string where each character indicates a particular
@ -- type of event to subscribe to.  Choices:
@ --     a - Announcements
@ --     c - Check-ins
@ --     t - Ticket changes
@ --     w - Wiki changes
@ -- Probably different codes will be added in the future.  In the future
@ -- we might also add a separate table that allows subscribing to email
@ -- notifications for specific branches or tags or tickets.
@ --
@ CREATE TABLE repository.subscriber(
@   subscriberId INTEGER PRIMARY KEY, -- numeric subscriber ID.  Internal use
@   subscriberCode BLOB UNIQUE,       -- UUID for subscriber.  External use
@   semail TEXT,                      -- email address
@   suname TEXT,                      -- corresponding USER entry
@   sverify BOOLEAN,                  -- email address verified
@   sdonotcall BOOLEAN,               -- true for Do Not Call 
@   sdigest BOOLEAN,                  -- true for daily digests only
@   ssub TEXT,                        -- baseline subscriptions
@   sctime DATE,                      -- When this entry was created. JulianDay
@   smtime DATE,                      -- Last change.  JulianDay
@   smip TEXT                         -- IP address of last change
@ );
@ CREATE INDEX repository.subscriberUname
@   ON subscriber(suname) WHERE suname IS NOT NULL;
@ 
@ -- Email notifications that need to be sent.
@ --
@ -- If the eventid key is an integer, then it corresponds to the
@ -- EVENT.OBJID table.  Other kinds of eventids are reserved for
@ -- future expansion.
@ --
@ CREATE TABLE repository.email_pending(
@   eventid ANY PRIMARY KEY,          -- Object that changed
@   sentSep BOOLEAN DEFAULT false,    -- individual emails sent
@   sentDigest BOOLEAN DEFAULT false  -- digest emails sent
@ ) WITHOUT ROWID;
@ 
@ -- Record bounced emails.  If too many bounces are received within
@ -- some defined time range, then cancel the subscription.  Older
@ -- entries are periodically purged.
@ --
@ CREATE TABLE repository.email_bounce(
@   subscriberId INTEGER, -- to whom the email was sent.
@   sendTime INTEGER,     -- seconds since 1970 when email was sent
@   rcvdTime INTEGER      -- seconds since 1970 when bounce was received
@ );
;

/*
** Make sure the unversioned table exists in the repository.
*/
void email_schema(void){
  if( !db_table_exists("repository", "subscriber") ){
    db_multi_exec(zEmailInit/*works-like:""*/);
  }
}


/*
** WEBPAGE: setup_email
**
** Administrative page for configuring and controlling email notification
*/
void setup_email(void){
  static const char *const azSendMethods[] = {
    "off",  "Disabled",
    "pipe", "Pipe to a command",
    "db",   "Store in a database",
    "file", "Store in a directory"
  };
  login_check_credentials();
  if( !g.perm.Setup ){
    login_needed(0);
    return;
  }
  db_begin_transaction();

  style_header("Email Notification Setup");
  @ <form action="%R/setup_email" method="post"><div>
  @ <input type="submit"  name="submit" value="Apply Changes" /><hr>
  login_insert_csrf_secret();
  multiple_choice_attribute("Email Send Method","email-send-method",
       "esm", "off", count(azSendMethods)/2, azSendMethods);
  @ <p>How to send email.  The "Pipe to a command"
  @ method is the usual choice in production.
  @ (Property: "email-send-method")</p>
  @ <hr>
  entry_attribute("Command To Pipe Email To", 80, "esc",
                   "email-send-command", "sendmail -t", 0);
  @ <p>When the send method is "pipe to a command", this is the command
  @ that is run.  Email messages are piped into the standard input of this
  @ command.  The command is expected to extract the sender address,
  @ recepient addresses, and subject from the header of the piped email
  @ text.  (Property: "email-send-command")</p>

  entry_attribute("Database In Which To Store Email", 60, "esdb",
                   "email-send-db", "", 0);
  @ <p>When the send method is "store in a databaes", each email message is
  @ stored in an SQLite database file with the name given here.
  @ (Property: "email-send-db")</p>

  entry_attribute("Directory In Which To Store Email", 60, "esdir",
                   "email-send-dir", "", 0);
  @ <p>When the send method is "store in a directory", each email message is
  @ stored as a separate file in the directory shown here.
  @ (Property: "email-send-dir")</p>
  @ <hr>

  entry_attribute("\"From\" email address", 40, "ef",
                   "email-self", "", 0);
  @ <p>This is the email from which email notifications are sent.  The
  @ system administrator should arrange for emails sent to this address
  @ to be handed off to the "fossil email incoming" command so that Fossil
  @ can handle bounces. (Property: "email-self")</p>
  @ <hr>

  entry_attribute("Administrator email address", 40, "ea",
                   "email-admin", "", 0);
  @ <p>This is the email for the human administrator for the system.
  @ Abuse and trouble reports are send here.
  @ (Property: "email-admin")</p>
  @ <hr>
  @ <p><input type="submit"  name="submit" value="Apply Changes" /></p>
  @ </div></form>
  db_end_transaction(0);
  style_footer();
}

/*
** Encode pMsg as MIME base64 and append it to pOut
*/
static void append_base64(Blob *pOut, Blob *pMsg){
  int n, i, k;
  char zBuf[100];
  n = blob_size(pMsg);
  for(i=0; i<n; i+=54){
    k = translateBase64(blob_buffer(pMsg)+i, i+54<n ? 54 : n-i, zBuf);
    blob_append(pOut, zBuf, k);
    blob_append(pOut, "\r\n", 2);
  }
}

/*
** Send an email message using whatever sending mechanism is configured
** by these settings:
**
**   email-send-method    "off"   Do not send any emails
**                        "pipe"  Pipe the email to email-send-command
**                        "db"    Store the mail in database email-send-db
**                        "file"  Store the email as a file in email-send-dir 
**
** The recepient(s) must be specified using  "To:" or "Cc:" or "Bcc:" fields
** in the header.  Likewise, the header must contains a "Subject:" line.
** The header might also include fields like "Message-Id:" or
** "In-Reply-To:".
**
** This routine will add fields to the header as follows:
**
**     From:
**     Content-Type:
**     Content-Transfer-Encoding:
**     
** At least one body must be supplied.
**
** The caller maintains ownership of the input Blobs.  This routine will
** read the Blobs and send them onward to the email system, but it will
** not free them.
**
** If zDest is not NULL then it is an overwrite for the email-send-method.
** zDest can be "stdout" to send output to the console for debugging.
*/
void email_send(Blob *pHdr, Blob *pPlain, Blob *pHtml, const char *zDest){
  const char *zFrom = db_get("email-self", 0);
  char *zBoundary = 0;
  Blob all;
  if( zFrom==0 ){
    fossil_warning("Missing configuration: \"email-self\"");
    return;
  }
  if( zDest==0 ) zDest = db_get("email-send-method", "off");
  if( strcmp(zDest, "off")==0 ){
    return;
  }
  blob_init(&all, 0, 0);
  blob_append(&all, blob_buffer(pHdr), blob_size(pHdr));
  blob_appendf(&all, "From: %s\r\n", zFrom);
  if( pPlain && pHtml ){
    blob_appendf(&all, "MIME-Version: 1.0\r\n");
    zBoundary = db_text(0, "SELECT hex(randomblob(20))");
    blob_appendf(&all, "Content-Type: multipart/alternative;"
                       " boundary=\"%s\"\r\n", zBoundary);
  }
  if( pPlain ){
    if( zBoundary ){
      blob_appendf(&all, "\r\n--%s\r\n", zBoundary);
    }
    blob_appendf(&all,"Content-Type: text/plain\r\n");
    blob_appendf(&all, "Content-Transfer-Encoding: base64\r\n\r\n");
    append_base64(&all, pPlain);
  }
  if( pHtml ){
    if( zBoundary ){
      blob_appendf(&all, "--%s\r\n", zBoundary);
    }
    blob_appendf(&all,"Content-Type: text/html\r\n");
    blob_appendf(&all, "Content-Transfer-Encoding: base64\r\n\r\n");
    append_base64(&all, pHtml);
  }
  if( zBoundary ){
    blob_appendf(&all, "--%s--\r\n", zBoundary);
    fossil_free(zBoundary);
    zBoundary = 0;
  }
  if( strcmp(zDest, "db")==0 ){
    sqlite3 *db;
    sqlite3_stmt *pStmt;
    int rc;
    const char *zDb = db_get("email-send-db",0);
    rc = sqlite3_open(zDb, &db);
    if( rc==SQLITE_OK ){
      sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS email(\n"
                       "  emailid INTEGER PRIMARY KEY,\n"
                       "  msg TEXT\n);", 0, 0, 0);
      rc = sqlite3_prepare_v2(db, "INSERT INTO email(msg) VALUES(?1)", -1,
                              &pStmt, 0);
      if( rc==SQLITE_OK ){
        sqlite3_bind_text(pStmt, 1, blob_str(&all), -1, SQLITE_TRANSIENT);
        sqlite3_step(pStmt);
        sqlite3_finalize(pStmt);
      }
      sqlite3_close(db);
    }
  }else if( strcmp(zDest, "pipe")==0 ){
    const char *zCmd = db_get("email-send-command", 0);
    if( zCmd ){
      FILE *out = popen(zCmd, "w");
      if( out ){
        fwrite(blob_buffer(&all), 1, blob_size(&all), out);
        fclose(out);
      }
    }
  }else if( strcmp(zDest, "dir")==0 ){
    const char *zDir = db_get("email-send-dir","./");
    char *zFile = db_text(0,
        "SELECT %Q||strftime('/%%Y%%m%%d%%H%%M%%S','now')||hex(randomblob(8))",
        zDir);
    blob_write_to_file(&all, zFile);
    fossil_free(zFile);
  }else if( strcmp(zDest, "stdout")==0 ){
    fossil_print("%s\n", blob_str(&all));
  }
  blob_zero(&all);
}

/*
** SETTING: email-send-method         width=5 default=off
** Determine the method used to send email.  Allowed values are
** "off", "pipe", "dir", "db", and "stdout".  The "off" value means
** no email is ever sent.  The "pipe" value means email messages are
** piped into a command determined by the email-send-command setting.
** The "dir" value means emails are written to individual files in a
** directory determined by the email-send-dir setting.  The "db" value
** means that emails are added to an SQLite database named by the
** email-send-db setting.  The "stdout" value writes email text to
** standard output, for debugging.
*/
/*
** SETTING: email-send-command       width=40
** This is a command to which outbound email content is piped when the
** email-send-method is set to "pipe".  The command must extract
** recipient, sender, subject, and all other relevant information
** from the email header.
*/
/*
** SETTING: email-send-dir           width=40
** This is a directory into which outbound emails are written as individual
** files if the email-send-method is set to "dir".
*/
/*
** SETTING: email-send-db            width=40
** This is an SQLite database file into which outbound emails are written
** if the email-send-method is set to "db".
*/
/*
** SETTING: email-self               width=40
** This is the email address for the repository.  Outbound emails add
** this email address as the "From:" field.
*/


/*
** COMMAND: email
** 
** Usage: %fossil email SUBCOMMAND ARGS...
**
** Subcommands:
**
**    reset                   Hard reset of all email notification tables
**                            in the repository.  This erases all subscription
**                            information.  Use with extreme care.
**
**    send TO [OPTIONS]       Send a single email message using whatever
**                            email sending mechanism is currently configured.
**                            Use this for testing the email configuration.
**                            Options:
**
**                              --body FILENAME
**                              --html
**                              --stdout
**                              --subject|-S SUBJECT
**
**    settings [NAME VALUE]   With no arguments, list all email settings.
**                            Or change the value of a single email setting.
*/
void email_cmd(void){
  const char *zCmd;
  int nCmd;
  db_find_and_open_repository(0, 0);
  email_schema();
  zCmd = g.argc>=3 ? g.argv[2] : "x";
  nCmd = (int)strlen(zCmd);
  if( strncmp(zCmd, "reset", nCmd)==0 ){
    Blob yn;
    int c;
    fossil_print(
        "This will erase all content in the repository tables, thus\n"
        "deleting all subscriber information.  The information will be\n"
        "unrecoverable.\n");
    prompt_user("Continue? (y/N) ", &yn);
    c = blob_str(&yn)[0];
    if( c=='y' ){
      db_multi_exec(
        "DROP TABLE IF EXISTS subscriber;\n"
        "DROP TABLE IF EXISTS subscription;\n"
        "DROP TABLE IF EXISTS email_pending;\n"
        "DROP TABLE IF EXISTS email_bounce;\n"
      );
      email_schema();
    }
    blob_zero(&yn);
  }else
  if( strncmp(zCmd, "send", nCmd)==0 ){
    Blob prompt, body, hdr;
    int sendAsBoth = find_option("both",0,0)!=0;
    int sendAsHtml = find_option("html",0,0)!=0;
    const char *zDest = find_option("stdout",0,0)!=0 ? "stdout" : 0;
    int i;
    const char *zSubject = find_option("subject", "S", 1);
    const char *zSource = find_option("body", 0, 1);
    verify_all_options();
    blob_init(&prompt, 0, 0);
    blob_init(&body, 0, 0);
    blob_init(&hdr, 0, 0);
    for(i=3; i<g.argc; i++){
      blob_appendf(&hdr, "To: %s\n", g.argv[i]);
    }
    if( zSubject ){
      blob_appendf(&hdr, "Subject: %s\n", zSubject);
    }
    if( zSource ){
      blob_read_from_file(&body, zSource, ExtFILE);
    }else{
      prompt_for_user_comment(&body, &prompt);
    }
    if( sendAsHtml ){
      email_send(&hdr, 0, &body, zDest);
    }else if( sendAsBoth ){
      Blob html;
      blob_init(&html, 0, 0);
      blob_appendf(&html, "<pre>\n%h</pre>\n", blob_str(&body));
      email_send(&hdr, &body, &html, zDest);
      blob_zero(&html);
    }else{
      email_send(&hdr, &body, 0, zDest);
    }
    blob_zero(&hdr);
    blob_zero(&body);
    blob_zero(&prompt);
  }
  else if( strncmp(zCmd, "settings", nCmd)==0 ){
    int isGlobal = find_option("global",0,0)!=0;
    int i;
    int nSetting;
    const Setting *pSetting = setting_info(&nSetting);
    db_open_config(1, 0);
    verify_all_options();
    if( g.argc!=3 && g.argc!=5 ) usage("setting [NAME VALUE]");
    if( g.argc==5 ){
      const char *zLabel = g.argv[3];
      if( strncmp(zLabel, "email-", 6)!=0
       || (pSetting = db_find_setting(zLabel, 1))==0 ){
        fossil_fatal("not a valid email setting: \"%s\"", zLabel);
      }
      db_set(pSetting->name, g.argv[4], isGlobal);
      g.argc = 3;
    }
    pSetting = setting_info(&nSetting);
    for(; nSetting>0; nSetting--, pSetting++ ){
      if( strncmp(pSetting->name,"email-",6)!=0 ) continue;
      print_setting(pSetting);
    }
  }
  else{
    usage("reset|send|setting");
  }
}

/*
** WEBPAGE: subscribe
**
** Allow users to subscribe to email notifications, or to change or
** verify their subscription.
*/
void subscribe_page(void){
  int needCaptcha;
  unsigned int uSeed;
  const char *zDecoded;
  char *zCaptcha;

  login_check_credentials();
  if( !g.perm.EmailAlert ){
    login_needed(g.anon.EmailAlert);
    return;
  }
  if( login_is_individual()
   && db_exists("SELECT 1 FROM subscriber WHERE suname=%Q",g.zLogin)
  ){
    cgi_redirect("%R/alerts");
    return;
  }
  style_header("Email Subscription");
  needCaptcha = P("usecaptcha")!=0 || !login_is_individual();
  form_begin(0, "%R/subscribe");
  @ <table class="subscribe">
  @ <tr>
  @  <td class="form_label">Email&nbsp;Address:</td>
  @  <td><input type="text" name="e" value="%h(PD("e",""))" size="30"></td>
  @  <td></td>
  @ </tr>
  if( needCaptcha ){
    uSeed = captcha_seed();
    zDecoded = captcha_decode(uSeed);
    zCaptcha = captcha_render(zDecoded);
    @ <tr>
    @  <td class="form_label">Security Code:</td>
    @  <td><input type="text" name="captcha" value="" size="30">
    @  <input type="hidden" name="usecaptcha" value="1"></td>
    @  <input type="hidden" name="captchaseed" value="%u(uSeed)"></td>
    @ </tr>
  }
  if( g.perm.Admin ){
    @ <tr>
    @  <td class="form_label">User:</td>
    @  <td><input type="text" name="suname" value="%h(PD("suname",g.zLogin))" \
    @  size="30"></td>
    @  <td></td>
    @ </tr>
  }
  @ <tr>
  @  <td class="form_label">Options:</td>
  @  <td><label><input type="checkbox" name="sa" value="%d(P10("sa"))">\
  @  Announcements</label><br>
  @  <label><input type="checkbox" name="sc" value="%d(P01("sc"))">\
  @  Check-ins</label><br>
  @  <label><input type="checkbox" name="st" value="%d(P01("st"))">\
  @  Ticket changes</label><br>
  @  <label><input type="checkbox" name="sw" value="%d(P01("sw"))">\
  @  Wiki</label><br>
  @  <label><input type="checkbox" name="di" value="%d(P01("di"))">\
  @  Daily digest only</label><br></td>
  @ </tr>
  @ <tr>
  @  <td></td>
  @  <td><input type="submit" value="Submit"></td>
  @ </tr>
  @ </table>
  if( needCaptcha ){
    @ <div class="captcha"><table class="captcha"><tr><td><pre>
    @ %h(zCaptcha)
    @ </pre>
    @ Enter the 8 characters above in the "Security Code" box
    @ </td></tr></table></div>
  }
  @ </form>
  style_footer();
}

/*
** WEBPAGE: alerts
**
** Edit email alert and notification settings.
**
** The subscriber entry is identified in either of two ways:
**
**    (1)  The name= query parameter contains the subscriberCode.
**         
**    (2)  The user is logged into an account other than "nobody" or
**         "anonymous".  In that case the notification settings
**         associated with that account can be edited without needing
**         to know the subscriber code.
*/
void alerts_page(void){
  const char *zName = P("name");
  Stmt q;
  int sa, sc, st, sw;
  int sdigest, sdonotcall, sverify;
  const char *ssub;
  const char *semail;
  const char *sctime;
  const char *smtime;
  const char *smip;
  int i;


  login_check_credentials();
  if( !g.perm.EmailAlert ){
    cgi_redirect("subscribe");
    return;
  }
  if( zName==0 && login_is_individual() ){
    zName = db_text(0, "SELECT hex(subscriberCode) FROM subscriber"
                       " WHERE suname=%Q", g.zLogin);
  }
  if( zName==0 || !validate16(zName, -1) ){
    cgi_redirect("subscribe");
    return;
  }
  if( P("submit")!=0 && cgi_csrf_safe(1) ){
    int sdonotcall = PB("sdonotcall");
    int sdigest = PB("sdigest");
    char ssub[10];
    int nsub = 0;
    if( PB("sa") ) ssub[nsub++] = 'a';
    if( PB("sc") ) ssub[nsub++] = 'c';
    if( PB("st") ) ssub[nsub++] = 't';
    if( PB("sw") ) ssub[nsub++] = 'w';
    ssub[nsub] = 0;
    db_multi_exec(
      "UPDATE subscriber SET"
      " sdonotcall=%d,"
      " sdigest=%d,"
      " ssub=%Q,"
      " smtime=julianday('now'),"
      " smip=%Q"
      " WHERE subscriberCode=hextoblob(%Q)",
      sdonotcall,
      sdigest,
      ssub,
      g.zIpAddr,
      zName
    );
  }
  if( PB("dodelete") && P("delete")!=0 && cgi_csrf_safe(1) ){
    db_multi_exec(
      "DELETE FROM subscriber WHERE subscriberCode=hextoblob(%Q)",
      zName
    );
  }
  db_prepare(&q,
    "SELECT"
    "  semail,"
    "  sverify,"
    "  sdonotcall,"
    "  sdigest,"
    "  ssub,"
    "  datetime(sctime),"
    "  datetime(smtime),"
    "  smip"
    " FROM subscriber WHERE subscriberCode=hextoblob(%Q)", zName);
  if( db_step(&q)!=SQLITE_ROW ){
    db_finalize(&q);
    cgi_redirect("subscribe");
    return;
  }
  style_header("Update Subscription");
  semail = db_column_text(&q, 0);
  sverify = db_column_int(&q, 1);
  sdonotcall = db_column_int(&q, 2);
  sdigest = db_column_int(&q, 3);
  ssub = db_column_text(&q, 4);
  sa = strchr(ssub,'a')!=0;
  sc = strchr(ssub,'c')!=0;
  st = strchr(ssub,'t')!=0;
  sw = strchr(ssub,'w')!=0;
  sctime = db_column_text(&q, 5);
  smtime = db_column_text(&q, 6);
  smip = db_column_text(&q, 7);
  form_begin(0, "%R/alerts");
  @ <table class="subscribe">
  @ <tr>
  @  <td class="form_label">Email&nbsp;Address:</td>
  @  <td>%h(semail)</td>
  @ </tr>
  if( g.perm.Admin ){
    @ <tr>
    @  <td class='form_label'>IP Address:</td>
    @  <td>%h(smip)</td>
    @ </tr>
  }
  @ <tr>
  @  <td class="form_label">Options:</td>
  @  <td><label><input type="checkbox" name="sa" value="%d(sa)">\
  @  Announcements</label><br>
  @  <label><input type="checkbox" name="sc" value="%d(sc)">\
  @  Check-ins</label><br>
  @  <label><input type="checkbox" name="st" value="%d(st)">\
  @  Ticket changes</label><br>
  @  <label><input type="checkbox" name="sw" value="%d(sw)">\
  @  Wiki</label><br>
  @  <label><input type="checkbox" name="sdigest" value="%d(sdigest)">\
  @  Daily digest only</label><br>
  if( g.perm.Admin ){
    @  <label><input type="checkbox" name="sdonotcall" value="%d(sdonotcall)">\
    @  Do not call</label><br>
    @  <label><input type="checkbox" name="sverify" value="%d(sverify)">\
    @  Verified</label><br>
  }
  @ </td></tr>
  @ <tr>
  @  <td></td>
  @  <td><input type="submit" value="Submit"></td>
  @ </tr>
  @ <tr>
  @  <td></td>
  @  <td><label><input type="checkbox" name="dodelete" value="0">
  @  Delete this subscription</label>
  @  <input type="submit" name="delete" value="Delete"></td>
  @ </tr>
  @ </table>
  @ </form>
  db_finalize(&q);
  style_footer();
}