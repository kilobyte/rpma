// SPDX-License-Identifier: BSD-3-Clause
/* Copyright 2020, Intel Corporation */

/*
 * log_default.c -- the default logging function with support for logging either
 * to syslog or to stderr
 */

#include <stdarg.h>
#include <syslog.h>
#include <time.h>
#include <string.h>

#include "log_default.h"
#include "log_internal.h"

static const char *const rpma_log_level_names[] = {
	[RPMA_LOG_LEVEL_FATAL]	= "FATAL",
	[RPMA_LOG_LEVEL_ERROR]	= "ERROR",
	[RPMA_LOG_LEVEL_WARNING] = "WARNING",
	[RPMA_LOG_LEVEL_NOTICE]	= "NOTICE",
	[RPMA_LOG_LEVEL_INFO]	= "INFO",
	[RPMA_LOG_LEVEL_DEBUG]	= "DEBUG",
};

static const int rpma_log_level_syslog_severity[] = {
	[RPMA_LOG_LEVEL_FATAL]	= LOG_CRIT,
	[RPMA_LOG_LEVEL_ERROR]	= LOG_ERR,
	[RPMA_LOG_LEVEL_WARNING] = LOG_WARNING,
	[RPMA_LOG_LEVEL_NOTICE]	= LOG_NOTICE,
	[RPMA_LOG_LEVEL_INFO]	= LOG_INFO,
	[RPMA_LOG_LEVEL_DEBUG]	= LOG_DEBUG,
};

/*
 * get_timestamp_prefix -- provide actual time in a readable string
 *
 * ASSUMPTIONS:
 * - buf != NULL && buf_size >= 16
 */
static void
get_timestamp_prefix(char *buf, size_t buf_size)
{
	struct tm *info;
	char date[24];
	struct timespec ts;
	long usec;

	const char error_message[] = "[time error] ";

	if (clock_gettime(CLOCK_REALTIME, &ts) ||
	    (NULL == (info = localtime(&ts.tv_sec)))) {
		memcpy(buf, error_message, sizeof(error_message));
		return;
	}

	usec = ts.tv_nsec / 1000;
	if (!strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", info)) {
		memcpy(buf, error_message, sizeof(error_message));
		return;
	}

	if (snprintf(buf, buf_size, "[%s.%06ld] ", date, usec) < 0) {
		memcpy(buf, error_message, sizeof(error_message));
		return;
	}
}

/*
 * rpma_log_default_function -- default logging function used to log a message
 * to syslog and/or stderr
 *
 * The message is started with prefix composed from file, line, func parameters
 * followed by string pointed by format. If format includes format specifiers
 * (subsequences beginning with %), the additional arguments following format
 * are formatted and inserted in the message.
 *
 * ASSUMPTIONS:
 * - level >= RPMA_LOG_LEVEL_FATAL && level <= RPMA_LOG_LEVEL_DEBUG
 * - level <= Rpma_log_threshold[RPMA_LOG_THRESHOLD]
 * - file == NULL || (file != NULL && function != NULL)
 */
void
rpma_log_default_function(rpma_log_level level, const char *file_name,
	const int line_no, const char *function_name,
	const char *message_format, ...)
{
	char file_info_buffer[256] = "";
	const char *file_info = file_info_buffer;
	char message[1024] = "";
	const char file_info_error[] = "[file info error]: ";

	va_list arg;
	va_start(arg, message_format);
	if (vsnprintf(message, sizeof(message), message_format, arg) < 0) {
		va_end(arg);
		return;
	}
	va_end(arg);

	if (file_name) {
		/* extract base_file_name */
		const char *base_file_name = strrchr(file_name, '/');
		if (!base_file_name)
			base_file_name = file_name;
		else
			/* skip '/' */
			base_file_name++;

		if (snprintf(file_info_buffer, sizeof(file_info_buffer),
				"%s: %4d: %s: ", base_file_name, line_no,
				function_name) < 0) {
			file_info = file_info_error;
		}
	}

	/* assumed: level <= Rpma_log_threshold[RPMA_LOG_THRESHOLD] */
	syslog(rpma_log_level_syslog_severity[level], "%s*%s*: %s",
		file_info, rpma_log_level_names[level], message);

	if (level <= Rpma_log_threshold[RPMA_LOG_THRESHOLD_AUX]) {
		char times_tamp[45] = "";
		get_timestamp_prefix(times_tamp, sizeof(times_tamp));
		(void) fprintf(stderr, "%s%s*%s*: %s", times_tamp, file_info,
			rpma_log_level_names[level], message);
	}
}

/*
 * rpma_log_default_init -- open a connection to the system logger
 */
void
rpma_log_default_init(void)
{
	openlog("rpma", LOG_PID, LOG_LOCAL7);
}


/*
 * rpma_log_default_fini -- close the descriptor being used to write to
 * the system logger
 */
void
rpma_log_default_fini(void)
{
	closelog();
}
