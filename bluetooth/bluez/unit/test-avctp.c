/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2014  Intel Corporation. All rights reserved.
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>

#include <glib.h>

#include "src/shared/util.h"
#include "src/log.h"

#include "android/avctp.h"

struct test_pdu {
	bool valid;
	const uint8_t *data;
	size_t size;
};

struct test_data {
	char *test_name;
	struct test_pdu *pdu_list;
};

struct context {
	GMainLoop *main_loop;
	struct avctp *session;
	guint source;
	guint process;
	int fd;
	unsigned int pdu_offset;
	const struct test_data *data;
};

#define data(args...) ((const unsigned char[]) { args })

#define raw_pdu(args...)					\
	{							\
		.valid = true,					\
		.data = data(args),				\
		.size = sizeof(data(args)),			\
	}

#define define_test(name, function, args...)				\
	do {								\
		const struct test_pdu pdus[] = {			\
			args, { }					\
		};							\
		static struct test_data data;				\
		data.test_name = g_strdup(name);			\
		data.pdu_list = g_malloc(sizeof(pdus));			\
		memcpy(data.pdu_list, pdus, sizeof(pdus));		\
		g_test_add_data_func(name, &data, function);		\
	} while (0)

static void test_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	g_print("%s%s\n", prefix, str);
}

static void test_free(gconstpointer user_data)
{
	const struct test_data *data = user_data;

	g_free(data->test_name);
	g_free(data->pdu_list);
}

static gboolean context_quit(gpointer user_data)
{
	struct context *context = user_data;

	if (context->process > 0)
		g_source_remove(context->process);

	g_main_loop_quit(context->main_loop);

	return FALSE;
}

static gboolean send_pdu(gpointer user_data)
{
	struct context *context = user_data;
	const struct test_pdu *pdu;
	ssize_t len;

	pdu = &context->data->pdu_list[context->pdu_offset++];

	len = write(context->fd, pdu->data, pdu->size);

	if (g_test_verbose())
		util_hexdump('<', pdu->data, len, test_debug, "AVCTP: ");

	g_assert_cmpint(len, ==, pdu->size);

	context->process = 0;
	return FALSE;
}

static void context_process(struct context *context)
{
	if (!context->data->pdu_list[context->pdu_offset].valid) {
		context_quit(context);
		return;
	}

	context->process = g_idle_add(send_pdu, context);
}

static gboolean test_handler(GIOChannel *channel, GIOCondition cond,
							gpointer user_data)
{
	struct context *context = user_data;
	const struct test_pdu *pdu;
	unsigned char buf[512];
	ssize_t len;
	int fd;

	pdu = &context->data->pdu_list[context->pdu_offset++];

	if (cond & (G_IO_NVAL | G_IO_ERR | G_IO_HUP)) {
		context->source = 0;
		g_print("%s: cond %x\n", __func__, cond);
		return FALSE;
	}

	fd = g_io_channel_unix_get_fd(channel);

	len = read(fd, buf, sizeof(buf));

	g_assert(len > 0);

	if (g_test_verbose())
		util_hexdump('>', buf, len, test_debug, "AVCTP: ");

	g_assert_cmpint(len, ==, pdu->size);

	g_assert(memcmp(buf, pdu->data, pdu->size) == 0);

	context_process(context);

	return TRUE;
}

static struct context *create_context(uint16_t version, gconstpointer data)
{
	struct context *context = g_new0(struct context, 1);
	GIOChannel *channel;
	int err, sv[2];

	context->main_loop = g_main_loop_new(NULL, FALSE);
	g_assert(context->main_loop);

	err = socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv);
	g_assert(err == 0);

	context->session = avctp_new(sv[0], 672, 672, version);
	g_assert(context->session != NULL);

	channel = g_io_channel_unix_new(sv[1]);

	g_io_channel_set_close_on_unref(channel, TRUE);
	g_io_channel_set_encoding(channel, NULL, NULL);
	g_io_channel_set_buffered(channel, FALSE);

	context->source = g_io_add_watch(channel,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				test_handler, context);
	g_assert(context->source > 0);

	g_io_channel_unref(channel);

	context->fd = sv[1];
	context->data = data;

	return context;
}

static void destroy_context(struct context *context)
{
	if (context->source > 0)
		g_source_remove(context->source);

	avctp_shutdown(context->session);

	g_main_loop_unref(context->main_loop);

	test_free(context->data);
	g_free(context);
}

static void execute_context(struct context *context)
{
	g_main_loop_run(context->main_loop);

	destroy_context(context);
}

static size_t handler(struct avctp *session,
					uint8_t transaction, uint8_t *code,
					uint8_t *subunit, uint8_t *operands,
					size_t operand_count, void *user_data)
{
	DBG("transaction %d code %d subunit %d operand_count %zu",
		transaction, *code, *subunit, operand_count);

	g_assert_cmpint(transaction, ==, 0);
	g_assert_cmpint(*code, ==, 0);
	g_assert_cmpint(*subunit, ==, 0);
	g_assert_cmpint(operand_count, ==, 0);

	return operand_count;
}

static gboolean handler_response(struct avctp *session,
					uint8_t code, uint8_t subunit,
					uint8_t *operands, size_t operand_count,
					void *user_data)
{
	struct context *context = user_data;

	DBG("code 0x%02x subunit %d operand_count %zu", code, subunit,
								operand_count);

	g_assert_cmpint(code, ==, 0x0a);
	g_assert_cmpint(subunit, ==, 0);
	g_assert_cmpint(operand_count, ==, 0);

	return context_quit(context);
}

static void test_client(gconstpointer data)
{
	struct context *context = create_context(0x0100, data);

	avctp_send_vendordep_req(context->session, AVC_CTYPE_CONTROL, 0, NULL,
						0, handler_response, context);

	execute_context(context);
}

static void test_server(gconstpointer data)
{
	struct context *context = create_context(0x0100, data);

	if (g_str_equal(context->data->test_name, "/TP/NFR/BV-03-C")) {
		int ret;

		ret = avctp_register_pdu_handler(context->session,
					AVC_OP_VENDORDEP, handler, NULL);
		DBG("ret %d", ret);
		g_assert_cmpint(ret, !=, 0);
	}

	g_idle_add(send_pdu, context);

	execute_context(context);
}

static void test_dummy(gconstpointer data)
{
	struct context *context = create_context(0x0100, data);

	destroy_context(context);
}

int main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	if (g_test_verbose())
		__btd_log_init("*", 0);

	/* Connection Channel Management tests */

	/*
	 * Tests are checking that IUT is able to request establishing
	 * channels, since we already have connection through socketpair
	 * the tests are dummy.
	 */
	define_test("/TP/CCM/BV-01-C", test_dummy, raw_pdu(0x00));
	define_test("/TP/CCM/BV-02-C", test_dummy, raw_pdu(0x00));
	define_test("/TP/CCM/BV-03-C", test_dummy, raw_pdu(0x00));
	define_test("/TP/CCM/BV-04-C", test_dummy, raw_pdu(0x00));

	/* Non-Fragmented Messages tests */

	define_test("/TP/NFR/BV-01-C", test_client,
				raw_pdu(0x00, 0x11, 0x0e, 0x00, 0x00, 0x00));

	define_test("/TP/NFR/BV-02-C", test_server,
				raw_pdu(0x00, 0x11, 0x0e, 0x00, 0x00, 0x00),
				raw_pdu(0x02, 0x11, 0x0e, 0x0a, 0x00, 0x00));

	define_test("/TP/NFR/BV-03-C", test_server,
				raw_pdu(0x00, 0x11, 0x0e, 0x00, 0x00, 0x00),
				raw_pdu(0x02, 0x11, 0x0e, 0x00, 0x00, 0x00));

	define_test("/TP/NFR/BV-04-C", test_client,
				raw_pdu(0x00, 0x11, 0x0e, 0x00, 0x00, 0x00),
				raw_pdu(0x02, 0x11, 0x0e, 0x0a, 0x00, 0x00));

	define_test("/TP/NFR/BI-01-C", test_server,
				raw_pdu(0x00, 0xff, 0xff, 0x00, 0x00, 0x00),
				raw_pdu(0x03, 0xff, 0xff));

	return g_test_run();
}
