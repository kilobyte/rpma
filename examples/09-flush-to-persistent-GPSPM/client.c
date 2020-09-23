// SPDX-License-Identifier: BSD-3-Clause
/* Copyright 2020, Intel Corporation */

/*
 * client.c -- a client of the flush-to-persistent-GPSPM example
 *
 * Please see README.md for a detailed description of this example.
 */

#include <librpma.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef USE_LIBPMEM
#include <libpmem.h>
#define USAGE_STR "usage: %s <server_address> <port> [<pmem-path>]\n"
#else
#define USAGE_STR "usage: %s <server_address> <port>\n"
#endif /* USE_LIBPMEM */

#include "common-conn.h"
#include "flush-to-persistent-GPSPM.h"

/* Generated by the protocol buffer compiler from: GPSPM_flush.proto */
#include "GPSPM_flush.pb-c.h"

enum lang_t {en, es};

static const char *hello_str[] = {
	[en] = "Hello world!",
	[es] = "¡Hola Mundo!"
};

#define LANG_NUM	(sizeof(hello_str) / sizeof(hello_str[0]))

struct hello_t {
	enum lang_t lang;
	char str[KILOBYTE];
};

#define FLUSH_ID	(void *)0xF01D /* a random identifier */

static inline void
write_hello_str(struct hello_t *hello, enum lang_t lang)
{
	hello->lang = lang;
	strncpy(hello->str, hello_str[hello->lang], KILOBYTE - 1);
	hello->str[KILOBYTE - 1] = '\0';
}

static void
translate(struct hello_t *hello)
{
	printf("Translating...\n");
	enum lang_t lang = (enum lang_t)((hello->lang + 1) % LANG_NUM);
	write_hello_str(hello, lang);
}

int
main(int argc, char *argv[])
{
	/* validate parameters */
	if (argc < 3) {
		fprintf(stderr, USAGE_STR, argv[0]);
		return -1;
	}

	/* configure logging thresholds to see more details */
	rpma_log_set_threshold(RPMA_LOG_THRESHOLD, RPMA_LOG_LEVEL_INFO);
	rpma_log_set_threshold(RPMA_LOG_THRESHOLD_AUX, RPMA_LOG_LEVEL_INFO);

	/* read common parameters */
	char *addr = argv[1];
	char *port = argv[2];
	int ret;

	/* resources - memory region */
	void *mr_ptr = NULL;
	size_t mr_size = 0;
	size_t data_offset = 0;
	enum rpma_mr_plt mr_plt = RPMA_MR_PLT_VOLATILE;
	struct rpma_mr_remote *dst_mr = NULL;
	size_t dst_size = 0;
	size_t dst_offset = 0;
	struct rpma_mr_local *src_mr = NULL;
	struct rpma_completion cmpl;

	/* messaging resources */
	void *msg_ptr = NULL;
	void *send_ptr = NULL;
	void *recv_ptr = NULL;
	struct rpma_mr_local *msg_mr = NULL;
	GPSPMFlushRequest flush_req = GPSPM_FLUSH_REQUEST__INIT;
	size_t flush_req_size = 0;
	GPSPMFlushResponse *flush_resp = NULL;

	struct hello_t *hello = NULL;

#ifdef USE_LIBPMEM
	if (argc >= 4) {
		char *path = argv[3];
		int is_pmem;

		/* map the file */
		mr_ptr = pmem_map_file(path, 0 /* len */, 0 /* flags */,
				0 /* mode */, &mr_size, &is_pmem);
		if (mr_ptr == NULL)
			return -1;

		/* pmem is expected */
		if (!is_pmem) {
			(void) pmem_unmap(mr_ptr, mr_size);
			return -1;
		}

		/*
		 * At the beginning of the persistent memory, a signature is
		 * stored which marks its content as valid. So the length
		 * of the mapped memory has to be at least of the length of
		 * the signature to convey any meaningful content and be usable
		 * as a persistent store.
		 */
		if (mr_size < SIGNATURE_LEN) {
			(void) fprintf(stderr, "%s too small (%zu < %zu)\n",
					path, mr_size, SIGNATURE_LEN);
			(void) pmem_unmap(mr_ptr, mr_size);
			return -1;
		}
		data_offset = SIGNATURE_LEN;

		/*
		 * The space under the offset is intended for storing the hello
		 * structure. So the total space is assumed to be at least
		 * 1 KiB + offset of the string contents.
		 */
		if (mr_size - data_offset < sizeof(struct hello_t)) {
			fprintf(stderr, "%s too small (%zu < %zu)\n",
					path, mr_size, sizeof(struct hello_t));
			(void) pmem_unmap(mr_ptr, mr_size);
			return -1;
		}

		hello = (struct hello_t *)((uintptr_t)mr_ptr + data_offset);

		/*
		 * If the signature is not in place the persistent content has
		 * to be initialized and persisted.
		 */
		if (strncmp(mr_ptr, SIGNATURE_STR, SIGNATURE_LEN) != 0) {
			/* write an initial value and persist it */
			write_hello_str(hello, en);
			pmem_persist(hello, sizeof(struct hello_t));
			/* write the signature to mark the content as valid */
			memcpy(mr_ptr, SIGNATURE_STR, SIGNATURE_LEN);
			pmem_persist(mr_ptr, SIGNATURE_LEN);
		}

		mr_plt = RPMA_MR_PLT_PERSISTENT;
	}
#endif

	/* if no pmem support or it is not provided */
	if (mr_ptr == NULL) {
		mr_ptr = malloc_aligned(sizeof(struct hello_t));
		if (mr_ptr == NULL)
			return -1;

		mr_size = sizeof(struct hello_t);
		hello = mr_ptr;
		mr_plt = RPMA_MR_PLT_VOLATILE;

		/* write an initial value */
		write_hello_str(hello, en);
	}

	(void) printf("Next value: %s\n", hello->str);

	/* allocate messaging buffer */
	msg_ptr = malloc_aligned(KILOBYTE);
	if (msg_ptr == NULL) {
		ret = -1;
		goto err_free;
	}
	send_ptr = (char *)msg_ptr + SEND_OFFSET;
	recv_ptr = (char *)msg_ptr + RECV_OFFSET;

	/* RPMA resources */
	struct rpma_peer *peer = NULL;
	struct rpma_conn *conn = NULL;

	/*
	 * lookup an ibv_context via the address and create a new peer using it
	 */
	if ((ret = client_peer_via_address(addr, &peer)))
		goto err_free;

	/* establish a new connection to a server listening at addr:port */
	if ((ret = client_connect(peer, addr, port, NULL, &conn)))
		goto err_peer_delete;

	/* register the memory RDMA write */
	if ((ret = rpma_mr_reg(peer, mr_ptr, mr_size,
			RPMA_MR_USAGE_WRITE_SRC,
			mr_plt, &src_mr)))
		goto err_conn_disconnect;

	/* register the messaging memory */
	if ((ret = rpma_mr_reg(peer, msg_ptr, KILOBYTE,
			RPMA_MR_USAGE_SEND | RPMA_MR_USAGE_RECV,
			RPMA_MR_PLT_VOLATILE, &msg_mr))) {
		(void) rpma_mr_dereg(&src_mr);
		goto err_conn_disconnect;
	}

	/* obtain the remote side resources description */
	struct rpma_conn_private_data pdata;
	ret = rpma_conn_get_private_data(conn, &pdata);
	if (ret != 0 || pdata.len < sizeof(struct common_data))
		goto err_mr_dereg;

	/*
	 * Create a remote memory registration structure from the received
	 * descriptor.
	 */
	struct common_data *dst_data = pdata.ptr;

	if ((ret = rpma_mr_remote_from_descriptor(&dst_data->descriptors[0],
			dst_data->mr_desc_size, &dst_mr)))
		goto err_mr_dereg;

	/* get the remote memory region size */
	if ((ret = rpma_mr_remote_get_size(dst_mr, &dst_size))) {
		goto err_mr_remote_delete;
	} else if (dst_size < KILOBYTE) {
		fprintf(stderr,
				"Size of the remote memory region is too small for writing the data of the assumed size (%zu < %d)\n",
				dst_size, KILOBYTE);
		goto err_mr_remote_delete;
	}

	dst_offset = dst_data->data_offset;
	if ((ret = rpma_write(conn, dst_mr, dst_offset, src_mr,
			(data_offset + offsetof(struct hello_t, str)), KILOBYTE,
			RPMA_F_COMPLETION_ON_ERROR, NULL)))
		goto err_mr_remote_delete;

	/* prepare a response buffer */
	if ((ret = rpma_recv(conn, msg_mr, RECV_OFFSET, MSG_SIZE_MAX, NULL)))
		goto err_mr_remote_delete;

	/* prepare a flush message and pack it to a send buffer */
	flush_req.offset = dst_offset;
	flush_req.length = KILOBYTE;
	flush_req.op_context = (uint64_t)FLUSH_ID;
	flush_req_size = gpspm_flush_request__get_packed_size(&flush_req);
	if (flush_req_size > MSG_SIZE_MAX) {
		fprintf(stderr,
				"Packed flush request size is bigger than available send buffer space (%"
				PRIu64 " > %d\n", flush_req_size,
				MSG_SIZE_MAX);
		goto err_mr_remote_delete;
	}
	(void) gpspm_flush_request__pack(&flush_req, send_ptr);

	/* send the flush message */
	if ((ret = rpma_send(conn, msg_mr, SEND_OFFSET, flush_req_size,
			RPMA_F_COMPLETION_ON_ERROR, NULL)))
		goto err_mr_remote_delete;

	/* wait for the completion to be ready */
	if ((ret = rpma_conn_prepare_completions(conn)))
		goto err_mr_remote_delete;
	if ((ret = rpma_conn_next_completion(conn, &cmpl)))
		goto err_mr_remote_delete;

	/* validate the completion */
	if (cmpl.op_status != IBV_WC_SUCCESS)
		goto err_mr_remote_delete;
	if (cmpl.op != RPMA_OP_RECV) {
		(void) fprintf(stderr,
				"unexpected cmpl.op value "
				"(0x%" PRIXPTR " != 0x%" PRIXPTR ")\n",
				(uintptr_t)cmpl.op,
				(uintptr_t)RPMA_OP_RECV);
		goto err_mr_remote_delete;
	}

	/* unpack a response from the received buffer */
	flush_resp = gpspm_flush_response__unpack(NULL, cmpl.byte_len,
			recv_ptr);
	if (flush_resp == NULL) {
		fprintf(stderr, "Cannot unpack the flush response buffer\n");
		goto err_mr_remote_delete;
	}
	if (flush_resp->op_context != (uint64_t)FLUSH_ID) {
		(void) fprintf(stderr,
				"unexpected flush_resp->op_context value "
				"(0x%" PRIXPTR " != 0x%" PRIXPTR ")\n",
				(uintptr_t)flush_resp->op_context,
				(uintptr_t)FLUSH_ID);
		goto err_mr_remote_delete;
	}
	gpspm_flush_response__free_unpacked(flush_resp, NULL);

	/*
	 * Translate the message so the next time the greeting will be
	 * surprising.
	 */
	translate(hello);
#ifdef USE_LIBPMEM
	if (mr_plt == RPMA_MR_PLT_PERSISTENT) {
		pmem_persist(hello, sizeof(struct hello_t));
	}
#endif

	(void) printf("Translation: %s\n", hello->str);

err_mr_remote_delete:
	/* delete the remote memory region's structure */
	(void) rpma_mr_remote_delete(&dst_mr);

err_mr_dereg:
	(void) rpma_mr_dereg(&msg_mr);
	(void) rpma_mr_dereg(&src_mr);

err_conn_disconnect:
	(void) common_disconnect_and_wait_for_conn_close(&conn);

err_peer_delete:
	/* delete the peer */
	(void) rpma_peer_delete(&peer);

err_free:
	free(msg_ptr);

#ifdef USE_LIBPMEM
	if (mr_plt == RPMA_MR_PLT_PERSISTENT) {
		pmem_unmap(mr_ptr, mr_size);
		mr_ptr = NULL;
	}
#endif

	if (mr_ptr != NULL)
		free(mr_ptr);

	return ret;
}
