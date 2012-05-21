/*
Serval Rhizome file sharing
Copyright (C) 2012 The Serval Project
 
This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.
 
This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
 
You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include "serval.h"
#include "rhizome.h"
#include "strbuf.h"
#include <stdlib.h>

long long rhizome_space=0;
static const char *rhizome_thisdatastore_path = NULL;
static int rhizome_enabled_flag = -1; // unknown

int rhizome_enabled()
{
  if (rhizome_enabled_flag < 0)
    rhizome_enabled_flag = confValueGetBoolean("rhizome.enable", 1);
  return rhizome_enabled_flag;
}

const char *rhizome_datastore_path()
{
  if (!rhizome_thisdatastore_path)
    rhizome_set_datastore_path(NULL);
  return rhizome_thisdatastore_path;
}

int rhizome_set_datastore_path(const char *path)
{
  if (!path)
    path = confValueGet("rhizome.datastore_path", NULL);
  if (path) {
    rhizome_thisdatastore_path = strdup(path);
    if (path[0] != '/')
      WARNF("Dangerous rhizome.datastore_path setting: '%s' -- should be absolute", rhizome_thisdatastore_path);
  } else {
    rhizome_thisdatastore_path = serval_instancepath();
    WARNF("Rhizome datastore path not configured -- using instance path '%s'", rhizome_thisdatastore_path);
  }
  return 0;
}

int form_rhizome_datastore_path(char * buf, size_t bufsiz, const char *fmt, ...)
{
  strbuf b = strbuf_local(buf, bufsiz);
  strbuf_sprintf(b, "%s/", rhizome_datastore_path());  
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  strbuf_sprintf(b, fmt, ap2);
  va_end(ap);
  if (strbuf_overrun(b)) {
      WHY("Path buffer overrun");
      return 0;
  }
  return 1;
}

int create_rhizome_datastore_dir()
{
  if (debug & DEBUG_RHIZOME) DEBUGF("mkdirs(%s, 0700)", rhizome_datastore_path());
  return mkdirs(rhizome_datastore_path(), 0700);
}

sqlite3 *rhizome_db=NULL;

/* XXX Requires a messy join that might be slow. */
int rhizome_manifest_priority(char *id)
{
  long long result = sqlite_exec_int64("select max(grouplist.priorty) from grouplist,manifests,groupmemberships where manifests.id='%s' and grouplist.id=groupmemberships.groupid and groupmemberships.manifestid=manifests.id;",id);
  return result;
}

int rhizome_opendb()
{
  if (rhizome_db) return 0;

  if (create_rhizome_datastore_dir() == -1)
    return -1;
  char dbname[1024];
  if (!FORM_RHIZOME_DATASTORE_PATH(dbname, "rhizome.db"))
    return -1;

  if (sqlite3_open(dbname,&rhizome_db))
    return WHYF("SQLite could not open database: %s", sqlite3_errmsg(rhizome_db));

  /* Read Rhizome configuration */
  double rhizome_kb = atof(confValueGet("rhizome_kb", "1024"));
  rhizome_space = 1024LL * rhizome_kb;
  if (debug&DEBUG_RHIZOME) {
    DEBUGF("serval.conf:rhizome_kb=%.f", rhizome_kb);
    DEBUGF("Rhizome will use %lldB of storage for its database.", rhizome_space);
  }

  /* Create tables if required */
  if (sqlite3_exec(rhizome_db,"PRAGMA auto_vacuum=2;",NULL,NULL,NULL)) {
      WARNF("SQLite could enable incremental vacuuming: %s", sqlite3_errmsg(rhizome_db));
  }
  if (sqlite3_exec(rhizome_db,"CREATE TABLE IF NOT EXISTS GROUPLIST(id text not null primary key, closed integer,ciphered integer,priority integer);",NULL,NULL,NULL))
    {
      return WHYF("SQLite could not create GROUPLIST table: %s", sqlite3_errmsg(rhizome_db));
    }
  if (sqlite3_exec(rhizome_db,"CREATE TABLE IF NOT EXISTS MANIFESTS(id text not null primary key, manifest blob, version integer,inserttime integer, bar blob);",NULL,NULL,NULL))
    {
      return WHYF("SQLite could not create MANIFESTS table: %s", sqlite3_errmsg(rhizome_db));
    }
  if (sqlite3_exec(rhizome_db,"CREATE TABLE IF NOT EXISTS FILES(id text not null primary key, data blob, length integer, highestpriority integer, datavalid integer);",NULL,NULL,NULL))
    {
      return WHYF("SQLite could not create FILES table: %s", sqlite3_errmsg(rhizome_db));
    }
  if (sqlite3_exec(rhizome_db,"CREATE TABLE IF NOT EXISTS FILEMANIFESTS(fileid text not null, manifestid text not null);",NULL,NULL,NULL))
    {
      return WHYF("SQLite could not create FILEMANIFESTS table: %s", sqlite3_errmsg(rhizome_db));
    }
  if (sqlite3_exec(rhizome_db,"CREATE TABLE IF NOT EXISTS GROUPMEMBERSHIPS(manifestid text not null, groupid text not null);",NULL,NULL,NULL))
    {
      return WHYF("SQLite could not create GROUPMEMBERSHIPS table: %s", sqlite3_errmsg(rhizome_db));
    }
  if (sqlite3_exec(rhizome_db,"CREATE TABLE IF NOT EXISTS VERIFICATIONS(sid text not null, did text, name text, starttime integer, endtime integer, signature blob);",
		   NULL,NULL,NULL))
    {
      return WHYF("SQLite could not create VERIFICATIONS table: %s", sqlite3_errmsg(rhizome_db));
    }
  
  /* XXX Setup special groups, e.g., Serval Software and Serval Optional Data */

  return 0;
}

/* 
   Convenience wrapper for executing an SQL command that returns a single int64 value 
 */
long long sqlite_exec_int64(char *sqlformat,...)
{
  if (!rhizome_db) rhizome_opendb();

  va_list ap,ap2;
  char sqlstatement[8192];

  va_start(ap,sqlformat);
  va_copy(ap2,ap);

  vsnprintf(sqlstatement,8192,sqlformat,ap2); sqlstatement[8191]=0;

  va_end(ap);

  sqlite3_stmt *statement;
  switch (sqlite3_prepare_v2(rhizome_db,sqlstatement,-1,&statement,NULL))
    {
    case SQLITE_OK: case SQLITE_DONE: case SQLITE_ROW:
      break;
    default:
      sqlite3_finalize(statement);
      sqlite3_close(rhizome_db);
      rhizome_db=NULL;
      WHY(sqlstatement);
      WHY(sqlite3_errmsg(rhizome_db));
      return WHY("Could not prepare sql statement.");
    }
   if (sqlite3_step(statement) == SQLITE_ROW)
     {
       if (sqlite3_column_count(statement)!=1) {
	 sqlite3_finalize(statement);
	 return -1;
       }
       long long result= sqlite3_column_int64(statement,0);
       sqlite3_finalize(statement);
       return result;
     }
   sqlite3_finalize(statement);
   return 0;
}

long long rhizome_database_used_bytes()
{
  long long db_page_size=sqlite_exec_int64("PRAGMA page_size;");
  long long db_page_count=sqlite_exec_int64("PRAGMA page_count;");
  long long db_free_page_count=sqlite_exec_int64("PRAGMA free_count;");
  return db_page_size*(db_page_count-db_free_page_count);
}

int rhizome_make_space(int group_priority, long long bytes)
{
  sqlite3_stmt *statement;

  /* Asked for impossibly large amount */
  if (bytes>=(rhizome_space-65536)) return -1;

  long long db_used=rhizome_database_used_bytes(); 
  
  /* If there is already enough space now, then do nothing more */
  if (db_used<=(rhizome_space-bytes-65536)) return 0;

  /* Okay, not enough space, so free up some. */
  char sql[1024];
  snprintf(sql,1024,"select id,length from files where highestpriority<%d order by descending length",group_priority);
  if(sqlite3_prepare_v2(rhizome_db,sql, -1, &statement, NULL) != SQLITE_OK )
    {
      WHYF("SQLite error running query '%s': %s",sql,sqlite3_errmsg(rhizome_db));
      sqlite3_finalize(statement);
      sqlite3_close(rhizome_db);
      rhizome_db=NULL;
      exit(-1);
    }

  while ( bytes>(rhizome_space-65536-rhizome_database_used_bytes()) && sqlite3_step(statement) == SQLITE_ROW)
    {
      /* Make sure we can drop this blob, and if so drop it, and recalculate number of bytes required */
      const unsigned char *id;
      //long long length;

      /* Get values */
      if (sqlite3_column_type(statement, 0)==SQLITE_TEXT) id=sqlite3_column_text(statement, 0);
      else {
	WARNF("Incorrect type in id column of files table.");
	continue; }
      if (sqlite3_column_type(statement, 1)==SQLITE_INTEGER) ;//length=sqlite3_column_int(statement, 1);
      else {
	WARNF("Incorrect type in length column of files table.");
	continue; }
      
      /* Try to drop this file from storage, discarding any references that do not trump the priority of this
	 request.  The query done earlier should ensure this, but it doesn't hurt to be paranoid, and it also
	 protects against inconsistency in the database. */
      rhizome_drop_stored_file((char *)id,group_priority+1);
    }
  sqlite3_finalize(statement);

  //long long equal_priority_larger_file_space_used = sqlite_exec_int64("SELECT COUNT(length) FROM FILES WHERE highestpriority=%d and length>%lld",group_priority,bytes);
  /* XXX Get rid of any equal priority files that are larger than this one */

  /* XXX Get rid of any higher priority files that are not relevant in this time or location */

  /* Couldn't make space */
  return WHY("Incomplete");
}

/* Drop the specified file from storage, and any manifests that reference it, 
   provided that none of those manifests are being retained at a higher priority
   than the maximum specified here. */
int rhizome_drop_stored_file(char *id,int maximum_priority)
{
  char sql[1024];
  sqlite3_stmt *statement;
  int cannot_drop=0;

  if (strlen(id)>70) return -1;

  snprintf(sql,1024,"select manifests.id from manifests,filemanifests where manifests.id==filemanifests.manifestid and filemanifests.fileid='%s'",
	   id);
  if(sqlite3_prepare_v2(rhizome_db,sql, -1, &statement, NULL) != SQLITE_OK )
    {
      WHYF("SQLite error running query '%s': %s",sql,sqlite3_errmsg(rhizome_db));
      sqlite3_finalize(statement);
      sqlite3_close(rhizome_db);
      rhizome_db=NULL;
      return WHY("Could not drop stored file");
    }

  while ( sqlite3_step(statement) == SQLITE_ROW)
    {
      /* Find manifests for this file */
      const unsigned char *id;
      if (sqlite3_column_type(statement, 0)==SQLITE_TEXT) id=sqlite3_column_text(statement, 0);
      else {
	WHYF("Incorrect type in id column of manifests table.");
	continue; }
            
      /* Check that manifest is not part of a higher priority group.
	 If so, we cannot drop the manifest or the file.
         However, we will keep iterating, as we can still drop any other manifests pointing to this file
	 that are lower priority, and thus free up a little space. */
      if (rhizome_manifest_priority((char *)id)>maximum_priority) {
	cannot_drop=1;
      } else {
	printf("removing stale filemanifests, manifests, groupmemberships\n");
	sqlite_exec_int64("delete from filemanifests where manifestid='%s';",id);
	sqlite_exec_int64("delete from manifests where manifestid='%s';",id);
	sqlite_exec_int64("delete from keypairs where public='%s';",id);
	sqlite_exec_int64("delete from groupmemberships where manifestid='%s';",id);	
      }
    }
  sqlite3_finalize(statement);

  if (!cannot_drop) {
    printf("cleaning up filemanifests, manifests\n");
    sqlite_exec_int64("delete from filemanifests where fileid='%s';",id);
    sqlite_exec_int64("delete from files where id='%s';",id);
  }
  return 0;
}

/*
  Store the specified manifest into the sqlite database.
  We assume that sufficient space has been made for us.
  The manifest should be finalised, and so we don't need to
  look at the underlying manifest file, but can just write m->manifest_data
  as a blob.

  associated_filename needs to be read in and stored as a blob.  Hopefully that
  can be done in pieces so that we don't have memory exhaustion issues on small
  architectures.  However, we do know it's hash apriori from m, and so we can
  skip loading the file in if it is already stored.  mmap() apparently works on
  Linux FAT file systems, and is probably the best choice since it doesn't need
  all pages to be in RAM at the same time.

  SQLite does allow modifying of blobs once stored in the database.
  The trick is to insert the blob as all zeroes using a special function, and then
  substitute bytes in the blog progressively.

  We need to also need to create the appropriate row(s) in the MANIFESTS, FILES, 
  FILEMANIFESTS and GROUPMEMBERSHIPS tables, and possibly GROUPLIST as well.
 */
int rhizome_store_bundle(rhizome_manifest *m, const char *associated_filename)
{
  char sqlcmd[1024];
  const char *cmdtail;
  int i;

  char manifestid[crypto_sign_edwards25519sha512batch_PUBLICKEYBYTES * 2 + 1];
  strncpy(manifestid,rhizome_manifest_get(m,"id",NULL,0),64);
  manifestid[64]=0;
  for(i=0;i<64;i++)
    manifestid[i]=toupper(manifestid[i]);

  if (!m->finalised) return WHY("Manifest was not finalised");

  /* remove any old version of the manifest */
  if (sqlite_exec_int64("SELECT COUNT(*) FROM MANIFESTS WHERE id='%s';",manifestid)>0)
    {
      /* Manifest already exists.
	 Remove old manifest entry, and replace with new one.
	 But we do need to check if the file referenced by the old one is still needed,
	 and if it's priority is right */
      sqlite_exec_int64("DELETE FROM MANIFESTS WHERE id='%s';",manifestid);

      char sql[1024];
      sqlite3_stmt *statement;
      snprintf(sql,1024,"SELECT fileid from filemanifests where manifestid='%s';",
	       manifestid);
      if (sqlite3_prepare_v2(rhizome_db,sql,strlen(sql)+1,&statement,NULL)!=SQLITE_OK)
	{
	  WHY("sqlite3_prepare_v2() failed");
	  WHY(sql);
	  WHY(sqlite3_errmsg(rhizome_db));
	}
      else
	{
	  while ( sqlite3_step(statement)== SQLITE_ROW)
	    {
	      const unsigned char *fileid;
	      if (sqlite3_column_type(statement,0)==SQLITE_TEXT) {
		fileid=sqlite3_column_text(statement,0);
		rhizome_update_file_priority((char *)fileid);
	      }
	    }
	  sqlite3_finalize(statement);
	}      
      sqlite_exec_int64("DELETE FROM FILEMANIFESTS WHERE manifestid='%s';",manifestid);

    }

  /* Store manifest */
  if (debug & DEBUG_RHIZOME) DEBUGF("Writing into manifests table");
  snprintf(sqlcmd,1024,
	   "INSERT INTO MANIFESTS(id,manifest,version,inserttime,bar) VALUES('%s',?,%lld,%lld,?);",
	   manifestid, m->version, gettime_ms());

  if (m->haveSecret) {
    /* We used to store the secret in the database, but we don't anymore, as we use 
       the BK field in the manifest. So nothing to do here. */
  } else {
    /* We don't have the secret for this manifest, so only allow updates if 
       the self-signature is valid */
    if (!m->selfSigned) {
      WHY("*** Insert into manifests failed (-2).");
      return WHY("Manifest is not signed, and I don't have the key.  Manifest might be forged or corrupt.");
    }
  }

  sqlite3_stmt *statement;
  if (sqlite3_prepare_v2(rhizome_db,sqlcmd,strlen(sqlcmd)+1,&statement,&cmdtail) 
      != SQLITE_OK) {
    sqlite3_finalize(statement);
    WHY("*** Insert into manifests failed.");
    return WHY(sqlite3_errmsg(rhizome_db));
  }

  /* Bind manifest data to data field */
  if (sqlite3_bind_blob(statement,1,m->manifestdata,m->manifest_bytes,SQLITE_TRANSIENT)!=SQLITE_OK)
    {
      sqlite3_finalize(statement);
    WHY("*** Insert into manifests failed (2).");
      return WHY(sqlite3_errmsg(rhizome_db));
    }

  /* Bind BAR to data field */
  unsigned char bar[RHIZOME_BAR_BYTES];
  rhizome_manifest_to_bar(m,bar);
  
  if (sqlite3_bind_blob(statement,2,bar,RHIZOME_BAR_BYTES,SQLITE_TRANSIENT)
      !=SQLITE_OK)
    {
      sqlite3_finalize(statement);
    WHY("*** Insert into manifests failed (3).");
      return WHY(sqlite3_errmsg(rhizome_db));
    }

  if (rhizome_finish_sqlstatement(statement)) {
    WHY("*** Insert into manifests failed (4).");
    return WHY("SQLite3 failed to insert row for manifest");
  }
  else {
    if (debug & DEBUG_RHIZOME) DEBUGF("Insert into manifests apparently worked.");
  }

  /* Create relationship between file and manifest */
  long long r=sqlite_exec_int64("INSERT INTO FILEMANIFESTS(manifestid,fileid) VALUES('%s','%s');",
				 manifestid,
				 m->fileHexHash);
  if (r<0) {
    WHY(sqlite3_errmsg(rhizome_db));
    return WHY("SQLite3 failed to insert row in filemanifests.");
  }

  /* Create relationships to groups */
  if (rhizome_manifest_get(m,"isagroup",NULL,0)!=NULL) {
    /* This manifest is a group, so add entry to group list.
       Created group is not automatically subscribed to, however. */
    int closed=rhizome_manifest_get_ll(m,"closedgroup");
    if (closed<1) closed=0;
    int ciphered=rhizome_manifest_get_ll(m,"cipheredgroup");
    if (ciphered<1) ciphered=0;
    sqlite_exec_int64("delete from grouplist where id='%s';",manifestid);
    int storedP
      =sqlite_exec_int64("insert into grouplist(id,closed,ciphered,priority) VALUES('%s',%d,%d,%d);",
			 manifestid,closed,ciphered,RHIZOME_PRIORITY_DEFAULT);
    if (storedP<0) return WHY("Failed to insert group manifest into grouplist table.");
  }

  {
    int g;
    int dud=0;
    for(g=0;g<m->group_count;g++)
      {
	if (sqlite_exec_int64("INSERT INTO GROUPMEMBERSHIPS(manifestid,groupid) VALUES('%s','%s');",
			   manifestid, m->groups[g])<0)
	  dud++;
      }
    if (dud>0) return WHY("Failed to create one or more group associations");
  }

  /* Store the file */
  if (m->fileLength>0) 
    if (rhizome_store_file(associated_filename,m->fileHexHash,m->fileHighestPriority)) 
      return WHY("Could not store associated file");						   

  /* Get things consistent */
  sqlite3_exec(rhizome_db,"COMMIT;",NULL,NULL,NULL);

  return 0;
}

int rhizome_finish_sqlstatement(sqlite3_stmt *statement)
{
  /* Do actual insert, and abort if it fails */
  int dud=0;
  int r;
  r=sqlite3_step(statement);
  switch(r) {
  case SQLITE_DONE: case SQLITE_ROW: case SQLITE_OK:
    break;
  default:
    WHY("sqlite3_step() failed.");
    WHY(sqlite3_errmsg(rhizome_db));
    dud++;
    sqlite3_finalize(statement);
  }

  if ((!dud)&&((r=sqlite3_finalize(statement))!=SQLITE_OK)) {
    WHY("sqlite3_finalize() failed.");
    WHY(sqlite3_errmsg(rhizome_db));
    dud++;
  }

  if (dud)  return WHY("SQLite3 could not complete statement.");
  return 0;
}

/* Like sqlite_encode_binary(), but with a fixed rotation to make comparison of
   string prefixes easier.  Also, returns string directly for convenience.
   The rotoring through four return strings is so that this function can be used
   safely inline in sprintf() type functions, which makes composition of sql statements
   easier. */
int rse_rotor=0;
char rse_out[8][129];
char *rhizome_safe_encode(unsigned char *in,int len)
{
  char *r=rse_out[rse_rotor];
  rse_rotor++;
  rse_rotor&=7;

  int i,o=0;

  for(i=0;i<len;i++)
    {
      if (o<=127)
	switch(in[i])
	  {
	  case 0x00: case 0x01: case '\'':
	    r[o++]=0x01;
	    r[o++]=in[i]^0x80;
	    break;
	  default:
	    r[o++]=in[i];
	  }
    }
  r[128]=0;
  return r;
}

int rhizome_list_manifests(const char *service, const char *sender_sid, const char *recipient_sid, int limit, int offset)
{
  strbuf b = strbuf_alloca(1024);
  strbuf_sprintf(b,
      "SELECT files.id, files.length, manifests.id, manifests.manifest, manifests.version, manifests.inserttime"
      " FROM files, filemanifests, manifests"
      " WHERE files.id = filemanifests.fileid AND filemanifests.manifestid = manifests.id AND files.datavalid <> 0"
      " ORDER BY files.id ASC"
    );
  if (limit)
    strbuf_sprintf(b, " LIMIT %u", limit);
  if (offset)
    strbuf_sprintf(b, " OFFSET %u", offset);
  if (strbuf_overrun(b))
    return WHYF("SQL command too long: ", strbuf_str(b));
  sqlite3_stmt *statement;
  const char *cmdtail;
  int ret = 0;
  if (sqlite3_prepare_v2(rhizome_db, strbuf_str(b), strbuf_len(b) + 1, &statement, &cmdtail) != SQLITE_OK) {
    sqlite3_finalize(statement);
    ret = WHY(sqlite3_errmsg(rhizome_db));
  } else {
    size_t rows = 0;
    cli_puts("8"); cli_delim("\n"); // number of columns
    cli_puts("service"); cli_delim(":");
    cli_puts("fileid"); cli_delim(":");
    cli_puts("manifestid"); cli_delim(":");
    cli_puts("version"); cli_delim(":");
    cli_puts("inserttime"); cli_delim(":");
    cli_puts("length"); cli_delim(":");
    cli_puts("date"); cli_delim(":");
    cli_puts("name"); cli_delim("\n");
    while (sqlite3_step(statement) == SQLITE_ROW) {
      ++rows;
      if (!(   sqlite3_column_count(statement) == 6
	    && sqlite3_column_type(statement, 0) == SQLITE_TEXT
	    && sqlite3_column_type(statement, 1) == SQLITE_INTEGER
	    && sqlite3_column_type(statement, 2) == SQLITE_TEXT
	    && sqlite3_column_type(statement, 3) == SQLITE_BLOB
	    && sqlite3_column_type(statement, 4) == SQLITE_INTEGER
	    && sqlite3_column_type(statement, 5) == SQLITE_INTEGER
      )) { 
	ret = WHY("Incorrect statement column");
	break;
      }
      const char *manifestblob = (char *) sqlite3_column_blob(statement, 3);
      size_t manifestblobsize = sqlite3_column_bytes(statement, 3); // must call after sqlite3_column_blob()
      rhizome_manifest *m = rhizome_read_manifest_file(manifestblob, manifestblobsize, 0);
      const char *blob_service = rhizome_manifest_get(m, "service", NULL, 0);
      int match = 1;
      if (service[0] && !(blob_service && strcasecmp(service, blob_service) == 0))
	match = 0;
      if (match && sender_sid[0]) {
	const char *blob_sender = rhizome_manifest_get(m, "sender", NULL, 0);
	if (!(blob_sender && strcasecmp(sender_sid, blob_sender) == 0))
	  match = 0;
      }
      if (match && recipient_sid[0]) {
	const char *blob_recipient = rhizome_manifest_get(m, "recipient", NULL, 0);
	if (!(blob_recipient && strcasecmp(recipient_sid, blob_recipient) == 0))
	  match = 0;
      }
      if (match) {
	const char *blob_name = rhizome_manifest_get(m, "name", NULL, 0);
	long long blob_date = rhizome_manifest_get_ll(m, "date");
	cli_puts(blob_service ? blob_service : ""); cli_delim(":");
	cli_puts((const char *)sqlite3_column_text(statement, 0)); cli_delim(":");
	cli_puts((const char *)sqlite3_column_text(statement, 2)); cli_delim(":");
	cli_printf("%lld", (long long) sqlite3_column_int64(statement, 4)); cli_delim(":");
	cli_printf("%lld", (long long) sqlite3_column_int64(statement, 5)); cli_delim(":");
	cli_printf("%u", sqlite3_column_int(statement, 1)); cli_delim(":");
	cli_printf("%lld", blob_date); cli_delim(":");
	cli_puts(blob_name ? blob_name : ""); cli_delim("\n");
      }
      rhizome_manifest_free(m);
    }
  }
  sqlite3_finalize(statement);
  return ret;
}

/* The following function just stores the file (or silently returns if it already exists).
   The relationships of manifests to this file are the responsibility of the caller. */
int rhizome_store_file(const char *file,char *hash,int priority)
{
  int fd=open(file,O_RDONLY);
  if (fd<0) return WHY("Could not open associated file");
  
  struct stat stat;
  if (fstat(fd,&stat)) {
    close(fd);
    return WHY("Could not stat() associated file");
  }

  unsigned char *addr =
    mmap(NULL, stat.st_size, PROT_READ, MAP_FILE|MAP_SHARED, fd, 0);
  if (addr==MAP_FAILED) {
    close(fd);
    return WHY("mmap() of associated file failed.");
  }

  /* Get hash of file if not supplied */
  char hexhash[SHA512_DIGEST_STRING_LENGTH];
  if (!hash)
    {
      /* Hash the file */
      SHA512_CTX c;
      SHA512_Init(&c);
      SHA512_Update(&c,addr,stat.st_size);
      SHA512_End(&c,hexhash);
      hash=hexhash;
    }

  /* INSERT INTO FILES(id as text, data blob, length integer, highestpriority integer).
   BUT, we have to do this incrementally so that we can handle blobs larger than available memory. 
  This is possible using: 
     int sqlite3_bind_zeroblob(sqlite3_stmt*, int, int n);
  That binds an all zeroes blob to a field.  We can then populate the data by
  opening a handle to the blob using:
     int sqlite3_blob_write(sqlite3_blob *, const void *z, int n, int iOffset);
*/
  
  char sqlcmd[1024];
  const char *cmdtail;

  /* See if the file is already stored, and if so, don't bother storing it again */
  int count=sqlite_exec_int64("SELECT COUNT(*) FROM FILES WHERE id='%s' AND datavalid<>0;",hash); 
  if (count==1) {
    /* File is already stored, so just update the highestPriority field if required. */
    long long storedPriority = sqlite_exec_int64("SELECT highestPriority FROM FILES WHERE id='%s' AND datavalid!=0",hash);
    if (storedPriority<priority)
      {
	snprintf(sqlcmd,1024,"UPDATE FILES SET highestPriority=%d WHERE id='%s';",
		 priority,hash);
	if (sqlite3_exec(rhizome_db,sqlcmd,NULL,NULL,NULL)!=SQLITE_OK) {
	  close(fd);
	  WHY(sqlite3_errmsg(rhizome_db));
	  return WHY("SQLite failed to update highestPriority field for stored file.");
	}
      }
    close(fd);
    return 0;
  } else if (count>1) {
    /* This should never happen! */
    return WHY("Duplicate records for a file in the rhizome database.  Database probably corrupt.");
  }

  /* Okay, so there are no records that match, but we should delete any half-baked record (with datavalid=0) so that the insert below doesn't fail.
   Don't worry about the return result, since it might not delete any records. */
  sqlite3_exec(rhizome_db,"DELETE FROM FILES WHERE datavalid=0;",NULL,NULL,NULL);

  snprintf(sqlcmd,1024,"INSERT INTO FILES(id,data,length,highestpriority,datavalid) VALUES('%s',?,%lld,%d,0);",
	   hash,(long long)stat.st_size,priority);
  sqlite3_stmt *statement;
  if (sqlite3_prepare_v2(rhizome_db,sqlcmd,strlen(sqlcmd)+1,&statement,&cmdtail) 
      != SQLITE_OK)
    {
      close(fd);
      sqlite3_finalize(statement);
      return WHY(sqlite3_errmsg(rhizome_db));
    }
  
  /* Bind appropriate sized zero-filled blob to data field */
  int dud=0;
  int r;
  if ((r=sqlite3_bind_zeroblob(statement,1,stat.st_size))!=SQLITE_OK)
    {
      dud++;
      WHY("sqlite3_bind_zeroblob() failed");
      WHY(sqlite3_errmsg(rhizome_db));   
    }

  /* Do actual insert, and abort if it fails */
  if (!dud)
    switch(sqlite3_step(statement)) {
    case SQLITE_OK: case SQLITE_ROW: case SQLITE_DONE:
      break;
    default:
      dud++;
      WHY("sqlite3_step() failed");
      WHY(sqlite3_errmsg(rhizome_db));   
    }

  if (sqlite3_finalize(statement)) dud++;
  if (dud) {
    close(fd);
    if (sqlite3_finalize(statement)!=SQLITE_OK)
      {
	WHY("sqlite3_finalize() failed");
	WHY(sqlite3_errmsg(rhizome_db));
      }
    return WHY("SQLite3 failed to insert row for file");
  }

  /* Get rowid for inserted row, so that we can modify the blob */
  int rowid=sqlite3_last_insert_rowid(rhizome_db);
  if (rowid<1) {
    close(fd);
    WHY(sqlite3_errmsg(rhizome_db));
    return WHY("SQLite3 failed return rowid of inserted row");
  }

  sqlite3_blob *blob;
  if (sqlite3_blob_open(rhizome_db,"main","FILES","data",rowid,
		    1 /* read/write */,
			&blob) != SQLITE_OK)
    {
      WHY(sqlite3_errmsg(rhizome_db));
      close(fd);
      sqlite3_blob_close(blob);
      return WHY("SQLite3 failed to open file blob for writing");
    }

  {
    long long i;
    for(i=0;i<stat.st_size;i+=65536)
      {
	int n=65536;
	if (i+n>stat.st_size) n=stat.st_size-i;
	if (sqlite3_blob_write(blob,&addr[i],n,i) !=SQLITE_OK) dud++;
      }
  }
  
  close(fd);
  sqlite3_blob_close(blob);

  /* Mark file as up-to-date */
  sqlite_exec_int64("UPDATE FILES SET datavalid=1 WHERE id='%s';",
	   hash);


  if (dud) {
      WHY(sqlite3_errmsg(rhizome_db));
      return WHY("SQLite3 failed write all blob data");
  }

  return 0;
}

char *rhizome_bytes_to_hex(unsigned char *in,int byteCount)
{
  int i=0;

  if (byteCount>64) return "<too long>";

  rse_rotor++;
  rse_rotor&=7;

  for(i=0;i<byteCount*2;i++)
    {
      int d=nybltochar((in[i>>1]>>(4-4*(i&1)))&0xf);
      rse_out[rse_rotor][i]=d;
    }
  rse_out[rse_rotor][i]=0;
  return rse_out[rse_rotor++];
}

int rhizome_update_file_priority(char *fileid)
{
  /* Drop if no references */
  int referrers=sqlite_exec_int64("SELECT COUNT(*) FROM FILEMANIFESTS WHERE fileid='%s';",fileid);
  if (referrers==0)
    rhizome_drop_stored_file(fileid,RHIZOME_PRIORITY_HIGHEST+1);
  if (referrers>0) {
    /* It has referrers, so workout the highest priority of any referrer */
        int highestPriority=sqlite_exec_int64("SELECT max(grouplist.priority) FROM MANIFESTS,FILEMANIFESTS,GROUPMEMBERSHIPS,GROUPLIST where manifests.id=filemanifests.manifestid AND groupmemberships.manifestid=manifests.id AND groupmemberships.groupid=grouplist.id AND filemanifests.fileid='%s';",fileid);
    if (highestPriority>=0)
      sqlite_exec_int64("UPDATE files set highestPriority=%d WHERE id='%s';",
			highestPriority,fileid);
  }
  return 0;
}

/* Search the database for a manifest having the same name and payload content,
   and if the version is known, having the same version.

   @author Andrew Bettison <andrew@servalproject.com>
 */
int rhizome_find_duplicate(const rhizome_manifest *m, rhizome_manifest **found)
{
  if (!m->fileHashedP)
    return WHY("Manifest payload is not hashed");
  const char *service = rhizome_manifest_get(m, "service", NULL, 0);
  const char *name = NULL;
  const char *sender = NULL;
  const char *recipient = NULL;
  if (service == NULL) {
    return WHY("Manifest has no service");
  } else if (strcasecmp(service, RHIZOME_SERVICE_FILE) == 0) {
    name = rhizome_manifest_get(m, "name", NULL, 0);
    if (!name) return WHY("Manifest has no name");
  } else if (strcasecmp(service, RHIZOME_SERVICE_MESHMS) == 0) {
    sender = rhizome_manifest_get(m, "sender", NULL, 0);
    recipient = rhizome_manifest_get(m, "recipient", NULL, 0);
    if (!sender) return WHY("Manifest has no sender");
    if (!recipient) return WHY("Manifest has no recipient");
  } else {
    return WHYF("Unsupported service '%s'", service);
  }
  char sqlcmd[1024];
  char *s = sqlcmd;
  s += snprintf(s, &sqlcmd[sizeof sqlcmd] - s,
      "SELECT manifests.id, manifests.manifest, manifests.version FROM filemanifests, manifests"
      " WHERE filemanifests.manifestid = manifests.id AND filemanifests.fileid = ?"
    );
  if (m->version != -1 && s < &sqlcmd[sizeof sqlcmd])
    s += snprintf(s, sqlcmd + sizeof(sqlcmd) - s, " AND manifests.version = ?");
  if (s >= &sqlcmd[sizeof sqlcmd])
    return WHY("SQL command too long");
  int ret = 0;
  sqlite3_stmt *statement;
  const char *cmdtail;
  if (sqlite3_prepare_v2(rhizome_db, sqlcmd, strlen(sqlcmd) + 1, &statement, &cmdtail) != SQLITE_OK) {
    ret = WHY(sqlite3_errmsg(rhizome_db));
  } else {
    if (debug & DEBUG_RHIZOME) DEBUGF("fileHexHash = \"%s\"", m->fileHexHash);
    sqlite3_bind_text(statement, 1, m->fileHexHash, -1, SQLITE_STATIC);
    if (m->version != -1)
      sqlite3_bind_int64(statement, 2, m->version);
    size_t rows = 0;
    while (sqlite3_step(statement) == SQLITE_ROW) {
      ++rows;
      if (debug & DEBUG_RHIZOME) DEBUGF("Row %d", rows);
      if (!(   sqlite3_column_count(statement) == 3
	    && sqlite3_column_type(statement, 0) == SQLITE_TEXT
	    && sqlite3_column_type(statement, 1) == SQLITE_BLOB
	    && sqlite3_column_type(statement, 2) == SQLITE_INTEGER
      )) { 
	ret = WHY("Incorrect statement columns");
	break;
      }
      const char *q_manifestid = (const char *) sqlite3_column_text(statement, 0);
      size_t manifestidsize = sqlite3_column_bytes(statement, 0); // must call after sqlite3_column_text()
      if (manifestidsize != crypto_sign_edwards25519sha512batch_PUBLICKEYBYTES * 2) {
	ret = WHYF("Malformed manifest.id from query: %s", q_manifestid);
	break;
      }
      const char *manifestblob = (char *) sqlite3_column_blob(statement, 1);
      size_t manifestblobsize = sqlite3_column_bytes(statement, 1); // must call after sqlite3_column_blob()
      long long q_version = sqlite3_column_int64(statement, 2);
      rhizome_manifest *blob_m = rhizome_read_manifest_file(manifestblob, manifestblobsize, 0);
      const char *blob_service = rhizome_manifest_get(blob_m, "service", NULL, 0);
      const char *blob_id = rhizome_manifest_get(blob_m, "id", NULL, 0);
      long long blob_version = rhizome_manifest_get_ll(blob_m, "version");
      const char *blob_filehash = rhizome_manifest_get(blob_m, "filehash", NULL, 0);
      long long blob_filesize = rhizome_manifest_get_ll(blob_m, "filesize");
      if (debug & DEBUG_RHIZOME)
	DEBUGF("Consider manifest.service=%s manifest.id=%s manifest.version=%lld", blob_service, q_manifestid, blob_version);
      /* Perform consistency checks, because we're paranoid. */
      int inconsistent = 0;
      if (blob_id && strcasecmp(blob_id, q_manifestid)) {
	WARNF("MANIFESTS row id=%s has inconsistent blob with id=%s -- skipped", q_manifestid, blob_id);
	++inconsistent;
      }
      if (blob_version != -1 && blob_version != q_version) {
	WARNF("MANIFESTS row id=%s has inconsistent blob: manifests.version=%lld, blob.version=%lld -- skipped",
	      q_manifestid, q_version, blob_version);
	++inconsistent;
      }
      if (!blob_filehash && strcmp(blob_filehash, m->fileHexHash)) {
	WARNF("MANIFESTS row id=%s joined to FILES row id=%s has inconsistent blob: blob.filehash=%s -- skipped",
	      q_manifestid, m->fileHexHash, blob_filehash);
	++inconsistent;
      }
      if (blob_filesize != -1 && blob_filesize != m->fileLength) {
	WARNF("MANIFESTS row id=%s joined to FILES row id=%s has inconsistent blob: known file size %lld, blob.filesize=%lld -- skipped",
	      q_manifestid, m->fileLength, blob_filesize);
	++inconsistent;
      }
      if (m->version != -1 && q_version != m->version) {
	WARNF("SELECT query with version=%lld returned incorrect row: manifests.version=%lld -- skipped", m->version, q_version);
	++inconsistent;
      }
      if (blob_service == NULL) {
	WARNF("MANIFESTS row id=%s has blob with no 'service' -- skipped", q_manifestid, blob_id);
	++inconsistent;
      }
      if (!inconsistent) {
	strbuf b = strbuf_alloca(1024);
	if (strcasecmp(service, RHIZOME_SERVICE_FILE) == 0) {
	  const char *blob_name = rhizome_manifest_get(blob_m, "name", NULL, 0);
	  if (blob_name && !strcmp(blob_name, name)) {
	    if (debug & DEBUG_RHIZOME)
	      strbuf_sprintf(b, " name=\"%s\"", blob_name);
	    ret = 1;
	  }
	} else if (strcasecmp(service, RHIZOME_SERVICE_FILE) == 0) {
	  const char *blob_sender = rhizome_manifest_get(blob_m, "sender", NULL, 0);
	  const char *blob_recipient = rhizome_manifest_get(blob_m, "recipient", NULL, 0);
	  if (blob_sender && !strcasecmp(blob_sender, sender) && blob_recipient && !strcasecmp(blob_recipient, recipient)) {
	    if (debug & DEBUG_RHIZOME)
	      strbuf_sprintf(b, " sender=%s recipient=%s", blob_sender, blob_recipient);
	    ret = 1;
	  }
	}
	if (ret == 1) {
	  rhizome_hex_to_bytes(q_manifestid, blob_m->cryptoSignPublic, crypto_sign_edwards25519sha512batch_PUBLICKEYBYTES*2); 
	  memcpy(blob_m->fileHexHash, m->fileHexHash, SHA512_DIGEST_STRING_LENGTH);
	  blob_m->fileHashedP = 1;
	  blob_m->fileLength = m->fileLength;
	  blob_m->version = q_version;
	  *found = blob_m;
	  DEBUGF("Found duplicate payload: service=%s%s version=%llu hexhash=%s",
		  blob_service, strbuf_str(b), blob_m->version, blob_m->fileHexHash
		);
	  break;
	}
      }
      rhizome_manifest_free(blob_m);
    }
  }
  sqlite3_finalize(statement);
  return ret;
}

/* Retrieve a manifest from the database, given its manifest ID.
 *
 * Returns 1 if manifest is found (if mp != NULL then a new manifest struct is allocated, made
 * finalisable and * assigned to *mp, caller is responsible for freeing).
 * Returns 0 if manifest is not found (*mp is unchanged).
 * Returns -1 on error (*mp is unchanged).
 */
int rhizome_retrieve_manifest(const char *manifestid, rhizome_manifest **mp)
{
  char sqlcmd[1024];
  int n = snprintf(sqlcmd, sizeof(sqlcmd), "SELECT id, manifest, version, inserttime FROM manifests WHERE id = ?");
  if (n >= sizeof(sqlcmd))
    return WHY("SQL command too long");
  sqlite3_stmt *statement;
  const char *cmdtail;
  int ret = 0;
  rhizome_manifest *m = NULL;
  if (sqlite3_prepare_v2(rhizome_db, sqlcmd, strlen(sqlcmd) + 1, &statement, &cmdtail) != SQLITE_OK) {
    sqlite3_finalize(statement);
    ret = WHY(sqlite3_errmsg(rhizome_db));
  } else {
    sqlite3_bind_text(statement, 1, manifestid, crypto_sign_edwards25519sha512batch_PUBLICKEYBYTES * 2, SQLITE_STATIC);
    while (sqlite3_step(statement) == SQLITE_ROW) {
      if (!(   sqlite3_column_count(statement) == 4
	    && sqlite3_column_type(statement, 0) == SQLITE_TEXT
	    && sqlite3_column_type(statement, 1) == SQLITE_BLOB
	    && sqlite3_column_type(statement, 2) == SQLITE_INTEGER
	    && sqlite3_column_type(statement, 3) == SQLITE_INTEGER
      )) { 
	ret = WHY("Incorrect statement column");
	break;
      }
      const char *manifestblob = (char *) sqlite3_column_blob(statement, 1);
      size_t manifestblobsize = sqlite3_column_bytes(statement, 1); // must call after sqlite3_column_blob()
      if (mp) {
	m = rhizome_read_manifest_file(manifestblob, manifestblobsize, 0);
	if (m == NULL) {
	  ret = WHY("Invalid manifest blob from database");
	} else {
	  ret = 1;
	  rhizome_hex_to_bytes(manifestid, m->cryptoSignPublic, crypto_sign_edwards25519sha512batch_PUBLICKEYBYTES*2); 
	  const char *blob_service = rhizome_manifest_get(m, "service", NULL, 0);
	  if (blob_service == NULL)
	    ret = WHY("Manifest is missing 'service' field");
	  const char *blob_filehash = rhizome_manifest_get(m, "filehash", NULL, 0);
	  if (blob_filehash == NULL)
	    ret = WHY("Manifest is missing 'filehash' field");
	  else {
	    memcpy(m->fileHexHash, blob_filehash, SHA512_DIGEST_STRING_LENGTH);
	    m->fileHashedP = 1;
	  }
	  long long blob_version = rhizome_manifest_get_ll(m, "version");
	  if (blob_version == -1)
	    ret = WHY("Manifest is missing 'version' field");
	  else
	    m->version = blob_version;
	  long long filesizeq = rhizome_manifest_get_ll(m, "filesize");
	  if (filesizeq == -1)
	    ret = WHY("Manifest is missing 'filesize' field");
	  else
	    m->fileLength = filesizeq;
	  if (ret == 1) {
	    cli_puts("service"); cli_delim(":");
	    cli_puts(blob_service); cli_delim("\n");
	    cli_puts("manifestid"); cli_delim(":");
	    cli_puts((const char *)sqlite3_column_text(statement, 0)); cli_delim("\n");
	    cli_puts("version"); cli_delim(":");
	    cli_printf("%lld", (long long) sqlite3_column_int64(statement, 2)); cli_delim("\n");
	    cli_puts("inserttime"); cli_delim(":");
	    cli_printf("%lld", (long long) sqlite3_column_int64(statement, 3)); cli_delim("\n");
	    cli_puts("filehash"); cli_delim(":");
	    cli_puts(m->fileHexHash); cli_delim("\n");
	    cli_puts("filesize"); cli_delim(":");
	    cli_printf("%lld", (long long) m->fileLength); cli_delim("\n");
	    // Could write the manifest blob to the CLI output here, but that would require the output to
	    // support byte[] fields as well as String fields.
	  }
	}
      }
      break;
    }
  }
  sqlite3_finalize(statement);
  if (mp && ret == 1)
    *mp = m;
  return ret;
}

/* Retrieve a file from the database, given its file hash.
 *
 * Returns 1 if file is found (contents are written to filepath if given).
 * Returns 0 if file is not found.
 * Returns -1 on error.
 */
int rhizome_retrieve_file(const char *fileid, const char *filepath)
{
  char sqlcmd[1024];
  int n = snprintf(sqlcmd, sizeof(sqlcmd), "SELECT id, data, length FROM files WHERE id = ? AND datavalid != 0");
  if (n >= sizeof(sqlcmd))
    return WHY("SQL command too long");
  sqlite3_stmt *statement;
  const char *cmdtail;
  int ret = 0;
  if (sqlite3_prepare_v2(rhizome_db, sqlcmd, strlen(sqlcmd) + 1, &statement, &cmdtail) != SQLITE_OK) {
    sqlite3_finalize(statement);
    ret = WHY(sqlite3_errmsg(rhizome_db));
  } else {
    sqlite3_bind_text(statement, 1, fileid, SHA512_DIGEST_LENGTH * 2, SQLITE_STATIC);
    while (sqlite3_step(statement) == SQLITE_ROW) {
      if (!(   sqlite3_column_count(statement) == 3
	    && sqlite3_column_type(statement, 0) == SQLITE_TEXT
	    && sqlite3_column_type(statement, 1) == SQLITE_BLOB
	    && sqlite3_column_type(statement, 2) == SQLITE_INTEGER
      )) { 
	ret = WHY("Incorrect statement column");
	break;
      }
      const char *fileblob = (char *) sqlite3_column_blob(statement, 1);
      size_t fileblobsize = sqlite3_column_bytes(statement, 1); // must call after sqlite3_column_blob()
      long long length = sqlite3_column_int64(statement, 2);
      if (fileblobsize != length)
	ret = WHY("File length does not match blob size");
      else {
	int fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0775);
	if (fd == -1) {
	  ret = WHYF("Cannot open %s for write/create", filepath);
	  WHY_perror("open");
	} else if (write(fd, fileblob, length) != length) {
	  ret = WHYF("Error writing %lld bytes to %s ", (long long) length, filepath);
	  WHY_perror("write");
	} else if (close(fd) == -1) {
	  ret = WHYF("Error flushing to %s ", filepath);
	  WHY_perror("close");
	} else {
	  ret = 1;
	  cli_puts("filehash"); cli_delim(":");
	  cli_puts((const char *)sqlite3_column_text(statement, 0)); cli_delim("\n");
	  cli_puts("filesize"); cli_delim(":");
	  cli_printf("%lld", length); cli_delim("\n");
	}
      }
      break;
    }
  }
  sqlite3_finalize(statement);
  return ret;
}
