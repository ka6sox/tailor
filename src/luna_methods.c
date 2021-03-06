/*=============================================================================
 Copyright (C) 2010 WebOS Internals <support@webos-internals.org>

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
 =============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <syslog.h>

#include "luna_service.h"
#include "luna_methods.h"

#define ALLOWED_CHARS "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-"

#define API_VERSION "1"

//
// We use static buffers instead of continually allocating and deallocating stuff,
// since we're a long-running service, and do not want to leak anything.
//
static char buffer[MAXBUFLEN];
static char esc_buffer[MAXBUFLEN];
static char run_command_buffer[MAXBUFLEN];
static char read_file_buffer[CHUNKSIZE+CHUNKSIZE+1];

typedef struct {
  LSMessage *message;
  FILE *fp;
} THREAD_DATA;

pthread_t resizeMediaThread = 0;

//
// Escape a string so that it can be used directly in a JSON response.
// In general, this means escaping quotes, backslashes and control chars.
// It uses the static esc_buffer, which must be twice as large as the
// largest string this routine can handle.
//
static char *json_escape_str(char *str)
{
  const char *json_hex_chars = "0123456789abcdef";

  // Initialise the output buffer
  strcpy(esc_buffer, "");

  // Check the constraints on the input string
  if (strlen(str) > MAXBUFLEN) return (char *)esc_buffer;

  // Initialise the pointers used to step through the input and output.
  char *resultsPt = (char *)esc_buffer;
  int pos = 0, start_offset = 0;

  // Traverse the input, copying to the output in the largest chunks
  // possible, escaping characters as we go.
  unsigned char c;
  do {
    c = str[pos];
    switch (c) {
    case '\0':
      // Terminate the copying
      break;
    case '\b':
    case '\n':
    case '\r':
    case '\t':
    case '"':
    case '\\': {
      // Copy the chunk before the character which must be escaped
      if (pos - start_offset > 0) {
	memcpy(resultsPt, str + start_offset, pos - start_offset);
	resultsPt += pos - start_offset;
      };
      
      // Escape the character
      if      (c == '\b') {memcpy(resultsPt, "\\b",  2); resultsPt += 2;} 
      else if (c == '\n') {memcpy(resultsPt, "\\n",  2); resultsPt += 2;} 
      else if (c == '\r') {memcpy(resultsPt, "\\r",  2); resultsPt += 2;} 
      else if (c == '\t') {memcpy(resultsPt, "\\t",  2); resultsPt += 2;} 
      else if (c == '"')  {memcpy(resultsPt, "\\\"", 2); resultsPt += 2;} 
      else if (c == '\\') {memcpy(resultsPt, "\\\\", 2); resultsPt += 2;} 

      // Reset the start of the next chunk
      start_offset = ++pos;
      break;
    }

    default:
      
      // Check for "special" characters
      if ((c < ' ') || (c > 127)) {

	// Copy the chunk before the character which must be escaped
	if (pos - start_offset > 0) {
	  memcpy(resultsPt, str + start_offset, pos - start_offset);
	  resultsPt += pos - start_offset;
	}

	// Insert a normalised representation
	sprintf(resultsPt, "\\u00%c%c",
		json_hex_chars[c >> 4],
		json_hex_chars[c & 0xf]);

	// Reset the start of the next chunk
	start_offset = ++pos;
      }
      else {
	// Just move along the source string, without copying
	pos++;
      }
    }
  } while (c);

  // Copy the final chunk, if required
  if (pos - start_offset > 0) {
    memcpy(resultsPt, str + start_offset, pos - start_offset);
    resultsPt += pos - start_offset;
  } 

  // Terminate the output buffer ...
  memcpy(resultsPt, "\0", 1);

  // and return a pointer to it.
  return (char *)esc_buffer;
}

//
// A dummy method, useful for unimplemented functions or as a status function.
// Called directly from webOS, and returns directly to webOS.
//
bool dummy_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);

  if (!LSMessageRespond(message, "{\"returnValue\": true}", &lserror)) goto error;

  return true;
 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

//
// Return the current API version of the service.
// Called directly from webOS, and returns directly to webOS.
//
bool version_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);

  if (!LSMessageRespond(message, "{\"returnValue\": true, \"version\": \"" VERSION "\", \"apiVersion\": \"" API_VERSION "\"}", &lserror)) goto error;

  return true;
 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

//
// Run a shell command, and return the output in-line in a buffer for returning to webOS.
// The global run_command_buffer must be initialised before calling this function.
// The return value says whether the command executed successfully or not.
//
static bool run_command(char *command, bool escape) {
  LSError lserror;
  LSErrorInit(&lserror);

  // Local buffers to store the current and previous lines.
  char line[MAXLINLEN];

  fprintf(stderr, "Running command %s\n", command);

  // run_command_buffer is assumed to be initialised, ready for strcat to append.

  // Is this the first line of output?
  bool first = true;

  bool array = false;

  // Start execution of the command, and read the output.
  FILE *fp = popen(command, "r");

  // Return immediately if we cannot even start the command.
  if (!fp) {
    return false;
  }

  // Loop through the output lines
  while (fgets(line, sizeof line, fp)) {

    // Chomp the newline
    char *nl = strchr(line,'\n'); if (nl) *nl = 0;

    // Add formatting breaks between lines
    if (first) {
      if (run_command_buffer[strlen(run_command_buffer)-1] == '[') {
	array = true;
      }
      first = false;
    }
    else {
      if (array) {
	strcat(run_command_buffer, ", ");
      }
      else {
	strcat(run_command_buffer, "<br>");
      }
    }
    
    // Append the unfiltered output to the run_command_buffer.
    if (escape) {
      if (array) {
	strcat(run_command_buffer, "\"");
      }
      strcat(run_command_buffer, json_escape_str(line));
      if (array) {
	strcat(run_command_buffer, "\"");
      }
    }
    else {
      strcat(run_command_buffer, line);
    }
  }
  
  // Check the close status of the process
  if (pclose(fp)) {
    return false;
  }

  return true;
 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  // %%% We need a way to distinguish command failures from LSMessage failures %%%
  // %%% This may need to be true if we just want to ignore LSMessage failures %%%
  return false;
}

//
// Send a standard format command failure message back to webOS.
// The command will be escaped.  The output argument should be a JSON array and is not escaped.
// The additional text  will not be escaped.
// The return value is from the LSMessageRespond call, not related to the command execution.
//
static bool report_command_failure(LSMessage *message, char *command, char *stdErrText, char *additional) {
  LSError lserror;
  LSErrorInit(&lserror);

  // Include the command that was executed, in escaped form.
  snprintf(buffer, MAXBUFLEN,
	   "{\"errorText\": \"Unable to run command: %s\"",
	   json_escape_str(command));

  // Include any stderr fields from the command.
  if (stdErrText) {
    strcat(buffer, ", \"stdErr\": ");
    strcat(buffer, stdErrText);
  }

  // Report that an error occurred.
  strcat(buffer, ", \"returnValue\": false, \"errorCode\": -1");

  // Add any additional JSON fields.
  if (additional) {
    strcat(buffer, ", ");
    strcat(buffer, additional);
  }

  // Terminate the JSON reply message ...
  strcat(buffer, "}");

  fprintf(stderr, "Message is %s\n", buffer);

  // and send it.
  if (!LSMessageRespond(message, buffer, &lserror)) goto error;

  return true;
 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

//
// Run a simple shell command, and return the output to webOS.
//
static bool simple_command(LSMessage *message, char *command) {
  LSError lserror;
  LSErrorInit(&lserror);

  // Initialise the output buffer
  strcpy(run_command_buffer, "{\"stdOut\": [");

  // Run the command
  if (run_command(command, true)) {

    // Finalise the message ...
    strcat(run_command_buffer, "], \"returnValue\": true}");

    fprintf(stderr, "Message is %s\n", run_command_buffer);

    // and send it to webOS.
    if (!LSMessageRespond(message, run_command_buffer, &lserror)) goto error;
  }
  else {

    // Finalise the command output ...
    strcat(run_command_buffer, "]");

    // and use it in a failure report message.
    if (!report_command_failure(message, command, run_command_buffer+11, NULL)) goto end;
  }

  return true;
 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

void resize_media_thread_cleanup(void *arg) {
  THREAD_DATA *data = (THREAD_DATA *)arg;

  syslog(LOG_DEBUG, "Resize thread cleanup, closing pipe %p, unref message %p\n", 
      data->fp, data->message);

  if (data->fp) pclose(data->fp);
  if (data->message) LSMessageUnref(data->message);
}


void *resize_media_thread(void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);
  char buffer[MAXBUFLEN];
  char esc_buffer[MAXBUFLEN];
  char command[MAXLINLEN];
  THREAD_DATA data;
  char line[MAXLINLEN];
  pthread_key_t key;

  data.message = (LSMessage *)ctx;

  // Extract the group argument from the message
  json_t *object = json_parse_document(LSMessageGetPayload(data.message));
  json_t *size = json_find_first_label(object, "size");               
  if (!size || (size->child->type != JSON_STRING) ||
      (strspn(size->child->text, ALLOWED_CHARS) != strlen(size->child->text))) {
    if (!LSMessageRespond(data.message,
			"{\"returnValue\": false, \"errorCode\": -1, \"errorText\": \"Invalid or missing size\"}",
			&lserror)) goto error;
    return NULL;
  }

  // Initialise the command
  sprintf(command, "/bin/resizefat -v /dev/mapper/store-media %s 2>&1", size->child->text);

  // Create thread key/value
  pthread_key_create(&key, resize_media_thread_cleanup);
  pthread_setspecific(key, &data);

  data.fp = popen(command, "r");
  syslog(LOG_DEBUG, "resize pipe fp %p\n", data.fp);

  if (!data.fp) {
    if (!LSMessageRespond(data.message, "{\"returnValue\": false, \"stage\": \"failed\"}", &lserror)) goto error;
    return NULL;
  }

  // Loop through the output lines
  while (fgets(line, sizeof line, data.fp)) {
    // Chomp the newline
    char *nl = strchr(line,'\n'); if (nl) *nl = 0;

    // Send it as a status message.
    strcpy(buffer, "{\"returnValue\": true, \"stage\": \"status\", \"status\": \"");
    strcat(buffer, json_escape_str(line));
    strcat(buffer, "\"}");

    // %%% Should we break out of the loop here, or just ignore the error? %%%
    if (!LSMessageRespond(data.message, buffer, &lserror)) goto error;

  }

  if (!LSMessageRespond(data.message, "{\"returnValue\": true, \"stage\": \"completed\"}", &lserror)) goto error;

  goto end;

 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return NULL;
}

//
// Run resizefat and provide the output back to Mojo
//
bool resize_media_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);

  if (resizeMediaThread) {
    syslog(LOG_NOTICE, "Resize thread already running\n");
    if (!LSMessageRespond(message, "{\"returnValue\": false, \"stage\": \"failed\"}", &lserror)) goto error;
    return true;
  }

  syslog(LOG_DEBUG, "Create resize thread, ref message %p\n", message);

  // Ref and save the message for use in resize thread
  LSMessageRef(message);

  if (pthread_create(&resizeMediaThread, NULL, resize_media_thread, (void*)message)) {
    syslog(LOG_ERR, "Creating resize thread failed (0x%x)\n", resizeMediaThread);
    // Report that the resize operation was not able to start
    if (!LSMessageRespond(message, "{\"returnValue\": false, \"errorCode\": -1, \"errorText\": \"Unable to start resize thread\"}", &lserror)) goto error;
  }
  else {
    syslog(LOG_DEBUG, "Created resize thread successfully (0x%x)\n", resizeMediaThread);
    // Report that the resize operation has begun
    if (!LSMessageRespond(message, "{\"returnValue\": true, \"stage\": \"start\"}", &lserror)) goto error;
  }

  return true;
 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

//
// Kill the currently running resize
//
bool kill_resize_media_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);

  if (!resizeMediaThread) {
    syslog(LOG_NOTICE, "Resize thread 0x%x not running\n", resizeMediaThread);
    if (!LSMessageRespond(message, "{\"returnValue\": false, \"stage\": \"failed\"}", &lserror)) goto error;
    return true;
  }

  syslog(LOG_DEBUG, "Killing resize thread 0x%x\n", resizeMediaThread);
  
  pthread_cancel(resizeMediaThread);
  resizeMediaThread = 0;

  if (!LSMessageRespond(message, "{\"returnValue\": true, \"stage\": \"completed\"}", &lserror)) goto error;

  return true;
 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

//
// Run command to retrieve volume group information.
//
bool list_groups_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);

  // Local buffer to store the command
  char command[MAXLINLEN];

  // Store the command, so it can be used in the error report if necessary
  sprintf(command, "/usr/sbin/vgdisplay -c 2>&1");
  
  return simple_command(message, command);

 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

//
// Run command to retrieve volume group information.
//
bool list_volumes_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);

  // Local buffer to store the command
  char command[MAXLINLEN];

  // Extract the group argument from the message
  json_t *object = json_parse_document(LSMessageGetPayload(message));
  json_t *group = json_find_first_label(object, "group");               
  if (!group || (group->child->type != JSON_STRING) ||
      (strspn(group->child->text, ALLOWED_CHARS) != strlen(group->child->text))) {
    if (!LSMessageRespond(message,
			"{\"returnValue\": false, \"errorCode\": -1, \"errorText\": \"Invalid or missing group\"}",
			&lserror)) goto error;
    return true;
  }

  // Store the command, so it can be used in the error report if necessary
  sprintf(command, "/usr/sbin/lvdisplay %s -c 2>&1", group->child->text);
  
  return simple_command(message, command);

 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

//
// Run command to get current mounts.
//
bool list_mounts_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);

  // Local buffer to store the command
  char command[MAXLINLEN];

  // Store the command, so it can be used in the error report if necessary
  sprintf(command, "cat /proc/mounts 2>&1");
  
  return simple_command(message, command);

 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

// Run command to retrieve filesystem usage
//
bool get_usage_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);

  // Local buffer to store the command
  char command[MAXLINLEN];

  // Extract the group argument from the message
  json_t *object = json_parse_document(LSMessageGetPayload(message));
  json_t *filesystem = json_find_first_label(object, "filesystem");
  if (!filesystem || (filesystem->child->type != JSON_STRING) ||
      (strspn(filesystem->child->text, ALLOWED_CHARS) != strlen(filesystem->child->text))) {
    if (!LSMessageRespond(message,
			"{\"returnValue\": false, \"errorCode\": -1, \"errorText\": \"Invalid or missing filesystem\"}",
			&lserror)) goto error;
    return true;
  }

  // Store the command, so it can be used in the error report if necessary
  sprintf(command, "/bin/df -m /dev/mapper/%s 2>&1", filesystem->child->text);
  
  return simple_command(message, command);

 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

// Run command to unmount a bind mount
//
bool unmount_bind__method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);

  // Local buffer to store the command
  char command[MAXLINLEN];

  // Extract the directory argument from the message
  json_t *object = json_parse_document(LSMessageGetPayload(message));
  json_t *directory = json_find_first_label(object, "directory");
  if (!directory || (directory->child->type != JSON_STRING) ||
      (strspn(directory->child->text, ALLOWED_CHARS) != strlen(directory->child->text))) {
    if (!LSMessageRespond(message,
			"{\"returnValue\": false, \"errorCode\": -1, \"errorText\": \"Invalid or missing directory\"}",
			&lserror)) goto error;
    return true;
  }

  // Store the command, so it can be used in the error report if necessary
  sprintf(command, "/bin/umount %s 2>&1", directory->child->text);
  
  return simple_command(message, command);

 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

//
// Run command to unmount the media partition.
//
bool unmount_media_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);

  // Local buffer to store the command
  char command[MAXLINLEN];

  // Store the command, so it can be used in the error report if necessary
  sprintf(command, "( /usr/bin/pkill -SIGUSR1 cryptofs && /bin/umount /media/internal ) 2>&1");
  
  return simple_command(message, command);

 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

//
// Run command to mount the media partition.
//
bool mount_media_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);

  // Local buffer to store the command
  char command[MAXLINLEN];

  // Store the command, so it can be used in the error report if necessary
  sprintf(command, "( /bin/mount /media/internal && /usr/bin/pkill -SIGUSR2 cryptofs ) 2>&1");
  
  return simple_command(message, command);

 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

//
// Run command to unmount the ext3fs partition.
//
bool unmount_ext3fs_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);

  // Local buffer to store the command
  char command[MAXLINLEN];

  // Store the command, so it can be used in the error report if necessary
  sprintf(command, "/bin/umount /media/ext3fs 2>&1");
  
  return simple_command(message, command);

 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

//
// Run command to mount the ext3fs partition.
//
bool mount_ext3fs_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);

  // Local buffer to store the command
  char command[MAXLINLEN];

  // Store the command, so it can be used in the error report if necessary
  sprintf(command, "/bin/mount /media/ext3fs 2>&1");
  
  return simple_command(message, command);

 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

//
// Run command to unmount the optware partition.
//
bool unmount_optware_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);

  // Local buffer to store the command
  char command[MAXLINLEN];

  // Store the command, so it can be used in the error report if necessary
  sprintf(command, "/sbin/stop org.webosinternals.optware 2>&1");
  
  return simple_command(message, command);

 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

//
// Run command to mount the optware partition.
//
bool mount_optware_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);

  // Local buffer to store the command
  char command[MAXLINLEN];

  // Store the command, so it can be used in the error report if necessary
  sprintf(command, "/sbin/start org.webosinternals.optware 2>&1");
  
  return simple_command(message, command);

 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

LSMethod luna_methods[] = {
  { "status",		dummy_method },
  { "version",		version_method },
  { "listGroups",	list_groups_method },
  { "listVolumes",	list_volumes_method },
  { "listMounts",	list_mounts_method },
  { "getUsage",		get_usage_method },
  { "unmountBind",	unmount_bind_method },
  { "unmountMedia",	unmount_media_method },
  { "resizeMedia",	resize_media_method },
  { "killResizeMedia",	kill_resize_media_method },
  //  { "reduceMedia",	reduce_media_method },
  //  { "extendMedia",	extend_media_method },
  { "mountMedia",	mount_media_method },
  { "unmountExt3fs",	unmount_ext3fs_method },
  { "mountExt3fs",	mount_ext3fs_method },
  { "unmountOptware",	unmount_optware_method },
  { "mountOptware",	mount_optware_method },
  { 0, 0 }
};

bool register_methods(LSPalmService *serviceHandle, LSError lserror) {
  return LSPalmServiceRegisterCategory(serviceHandle, "/", luna_methods,
				       NULL, NULL, NULL, &lserror);
}
