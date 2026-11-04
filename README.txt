UMLMM GATEWAY  -  SENDING (SMTP) AND RECEIVING (POP3)
=====================================================

URGENT FIX in this build: messages sent WITH ENCLOSURES had a broken
Content-Type header (the boundary was split onto its own line).  A strict
mail server could reject it or mishandle it.  Do not use earlier builds to
send enclosures.  Plain messages were not affected.

Changes in this build:
* Connection errors now include the WinSock error number, for example
  "cannot connect to server (WinSock error 10060)".  If a mail run ever
  cannot connect, send me that line.
* Incoming mail now also lists its other To: and Cc: recipients for Bob
  (turn off with ShowRecipients=0 in POP3.INI).
* umlmm.lnk has one new line (import WS_GETERR WINSOCK.111): use the new
  umlmm.lnk together with the new umlmm.c.

Earlier fix: Bob passes each incoming message a unique file name
(for example ...\IN\tnwrfr) and expects the engine to create that name with
.ENV / .MSG / .000 extensions.  The first receiving build mistook it for a
folder name.

What changed since the sending-only version
-------------------------------------------
* Receiving is now implemented.  On every mail run the gateway checks the
  POP3 mailbox of each user listed in Users= (POP3.INI), converts each
  message into the files Bob expects in MAILROOM\IN  (NNNNNNNN.ENV, .MSG and
  .000, .001 ... for attachments), and by default deletes it from the server.
* Sending is unchanged.

If a mail run was interrupted (crash, reset, VM killed)
-------------------------------------------------------
* Files that were being written at that moment can be left filled with
  zero bytes (all NUL).  Delete any file in MAILROOM\IN that is all zeros
  or empty, together with the files that share its name.
* Mail already downloaded is not lost: every message has a raw copy
  (<name>.EML) in C:\MSBOB\MAILROOM, and the server only removes mail when
  the session ends normally (QUIT).  If you are not sure, look at the mailbox
  on the server before doing anything else.
* To start a clean test, move everything out of MAILROOM\IN into another
  folder (for example C:\MSBOB\INBACKUP) so Bob does not try to import an
  old batch during the test.

Things to know first
--------------------
* The receiving side has been tested here only against a simulated server.
  I do not yet know exactly what Bob will accept in an incoming .ENV file,
  so the first real receive may need a round or two of adjustment.
* While testing, use a throwaway message sent from another account.  Delete=1
  removes mail from the server after download; KeepRaw=1 (default) saves a
  copy of every downloaded message as <name>.EML in C:\MSBOB\MAILROOM so
  nothing is lost if Bob refuses a file.
* Passwords are stored as plain text in POP3.INI.  Keep that file private,
  and change the mailbox password if you ever send the file to anyone.
* Keep your original UMLMM.DLL as UMLMM.ORG.

1. Build (same as before)
   Put umlmm.c, umlmm.lnk and build.bat in one folder (for example C:\GATE),
   open a Command Prompt there and run:  build.bat
   If the compiler complains about the "_asm" lines, run:  build.bat nodate
   If anything fails, send me the full messages.

2. Update POP3.INI   (REQUIRED - without Users= nothing is downloaded and
   MAILRUN.LOG says "No Users= line in [SERVER]")
   Compare your existing C:\MSBOB\MAILROOM\POP3.INI with POP3.INI.sample and
   add to the [SERVER] section:
       Users=218-0241          (all IDs, comma separated)
       Delete=1
       KeepRaw=1
       DateStyle=none
   and add to each [USER.<id>] section:
       FullName=Marisa Giancarla      (the name Bob shows for that user)
   Sending keeps working with the settings you already have.

3. Install
   Close Bob, wait about 10 seconds, then in C:\MSBOB:
       copy C:\GATE\UMLMM.DLL UMLMM.DLL      (answer Yes to overwrite)

4. Test receiving
   Use a SMALL test: in POP3.INI set
       MaxPerRun=3
       NewestFirst=1
   (newest first means your brand-new test message is the one downloaded
   first, even if the mailbox holds hundreds of old messages).  Remember that
   Delete=1 removes each downloaded message from the server.
   a. From another e-mail account, send a short plain-text test message to
      the user's address (marisa@msbob.org in the sample).
   b. In Bob, run mail.
   c. Check whether the message shows up in Bob.
   d. Whether it did or not, send me these files:
        C:\MSBOB\MAILROOM\UMLMM.TRC
        C:\MSBOB\MAILROOM\MAILRUN.LOG
      and, if present, the files left in C:\MSBOB\MAILROOM\IN (the .ENV and
      .MSG) and the .EML copy in C:\MSBOB\MAILROOM.
   Tell me exactly what Bob showed (new mail, nothing, an error message).

5. Date line experiment (only after step 4 works)
   Edit DateStyle= to  mci  and then to  rfc,  send one more test message each
   time, and tell me how Bob shows the date for each.  Change nothing else.

6. Restore the original
   Close Bob and run, in C:\MSBOB:   copy UMLMM.ORG UMLMM.DLL

What to look for in UMLMM.TRC
-----------------------------
  "Download target: ..."   the path Bob passed: <IN folder>\<unique name>;
                           the gateway creates <unique name>.ENV / .MSG / .000
  "Fallback IN folder"     used only if Bob passes something unexpected
  "Stored message 1"       a message was converted
  "Raw copy kept as ..."   the safety copy
  ERROR: lines             anything that went wrong (also in MAILRUN.LOG)
