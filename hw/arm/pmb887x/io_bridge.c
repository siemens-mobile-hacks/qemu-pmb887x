#include "hw/arm/pmb887x/io_bridge.h"

#ifdef PMB887X_IO_BRIDGE
#include <errno.h>
#include <stdio.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h> 
#include <sys/un.h>
#include <stdlib.h>
#include <stdint.h>

#include "hw/qdev-core.h"
#include "hw/irq.h"
#include "qemu/thread.h"
#include "qemu/main-loop.h"
#include "cpu.h"
#include "sysemu/cpu-timers.h"

#include "hw/arm/pmb887x/regs.h"

#define IO_BRIDGE_TIMEOUT	1000

static int sock_server_io = -1;
static int sock_server_irq = -1;

static int sock_client_io = -1;
static int sock_client_irq = -1;

static char cmd_w_size[] = {0, 'o', 'w', 'O', 'W'};
static char cmd_r_size[] = {0, 'i', 'r', 'I', 'R'};

static int _open_unix_sock(const char *name);
static int _wait_for_client(int sock);
static void _async_read(int sock, void *data, int size, int64_t timeout);
static void _async_write(int sock, void *data, int size, int64_t timeout);
static int _async_read_chunk(int sock, uint8_t *data, int size);
static int _async_write_chunk(int sock, uint8_t *data, int size);
static void *_irq_loop_thread(void *arg);

static DeviceState *nvic = NULL;

int current_irq = 0;

static QemuThread irq_trhead_id;

void pmb8876_io_bridge_init(void) {
	sock_server_io = _open_unix_sock("/dev/shm/pmb8876_io_bridge.sock");
	sock_server_irq = _open_unix_sock("/dev/shm/pmb8876_io_bridge_irq.sock");
	
	if (sock_server_io < 0 || sock_server_irq < 0) {
		fprintf(stderr, "[io bridge] Can't open sockets...\r\n");
		exit(1);
	}
	
	sock_client_io = _wait_for_client(sock_server_io);
	sock_client_irq = _wait_for_client(sock_server_irq);
	
	if (sock_server_io < 0 || sock_server_irq < 0) {
		fprintf(stderr, "[io bridge] Can't wait clients...\r\n");
		exit(1);
	}
	
	qemu_thread_create(&irq_trhead_id, "irq_loop", _irq_loop_thread, NULL, QEMU_THREAD_JOINABLE);
	
	fprintf(stderr, "[io bridge] IO bridge started...\r\n");
}

static void *_irq_loop_thread(void *arg) {
	while (true) {
		uint8_t irq;
		_async_read(sock_client_irq, &irq, 1, 0);
		
		bool locked = bql_locked();
		if (!locked)
			bql_lock();
		
		if (irq) {
			qemu_set_irq(qdev_get_gpio_in(nvic, irq), 100000);
			current_irq = irq;
		}
		
		if (!locked)
			bql_unlock();
	}
	return NULL;
}

void pmb8876_io_bridge_set_nvic(DeviceState *nvic_ref) {
	nvic = nvic_ref;
}

unsigned int pmb8876_io_bridge_read(unsigned int addr, unsigned int size) {
	cpu_disable_ticks();
	
	uint32_t from = ARM_CPU(qemu_get_cpu(0))->env.regs[15];
	
	cpu_disable_ticks();
	
	if (from % 4 == 0)
		from -= 4;
	else
		from -= 2;
	
	_async_write(sock_client_io, &cmd_r_size[size], 1, IO_BRIDGE_TIMEOUT);
	_async_write(sock_client_io, &addr, 4, IO_BRIDGE_TIMEOUT);
	_async_write(sock_client_io, &from, 4, IO_BRIDGE_TIMEOUT);
	
	uint8_t buf[5];
	_async_read(sock_client_io, &buf, 5, IO_BRIDGE_TIMEOUT);
	
	if (buf[0] != 0x21) {
		fprintf(stderr, "[io bridge] invalid ACK: %02X\n", buf[0]);
		exit(1);
	}
	
	cpu_enable_ticks();
	
	return buf[4] << 24 | buf[3] << 16 | buf[2] << 8 | buf[1];
}

void pmb8876_io_bridge_write(unsigned int addr, unsigned int size, unsigned int value) {
	cpu_disable_ticks();
	
	uint32_t from = ARM_CPU(qemu_get_cpu(0))->env.regs[15];
	
	if (from % 4 == 0)
		from -= 4;
	else
		from -= 2;
	
	/*
	if (addr == 0xF280020C && value)
		value = 1;
	
	if (addr == 0xF2800210 && value)
		value = 1;
	*/
	
	if ((addr == PMB8876_I2C_BASE + I2C_CLC) && (value & 1)) {
		value = 0x100;
	}
	
	if ((addr == 0xF1300000) && (value & 1)) {
		value = 0x100;
	}
	
	_async_write(sock_client_io, &cmd_w_size[size], 1, IO_BRIDGE_TIMEOUT);
	_async_write(sock_client_io, &addr, 4, IO_BRIDGE_TIMEOUT);
	_async_write(sock_client_io, &value, 4, IO_BRIDGE_TIMEOUT);
	_async_write(sock_client_io, &from, 4, IO_BRIDGE_TIMEOUT);
	
	uint8_t buf;
	_async_read(sock_client_io, &buf, 1, IO_BRIDGE_TIMEOUT);
	
	if (buf != 0x21) {
		fprintf(stderr, "[io bridge] invalid ACK: %02X\n", buf);
		exit(1);
	}
	
	cpu_enable_ticks();
}

static int _open_unix_sock(const char *name) {
	int sock;
	if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
		perror("[io bridge] socket");
		return -1;
	}
	
	struct sockaddr_un sock_un;
	memset(&sock_un, 0, sizeof(struct sockaddr_un));
	
	sock_un.sun_family = AF_UNIX;
	strcpy(sock_un.sun_path, name);
	
	unlink(name);
	
	int socket_length = strlen(sock_un.sun_path) + sizeof(sock_un.sun_family);
	if (bind(sock, (struct sockaddr *) &sock_un, socket_length) < 0) {
		perror("[io bridge] bind");
		close(sock);
		return -1;
	}
	
	if (listen(sock, 1) < 0) {
		perror("[io bridge] listen");
		close(sock);
		return -1;
	}
	
	return sock;
}

static int _wait_for_client(int sock) {
	struct pollfd pfd[1] = {
		{.fd = sock, .events = POLLIN},
	};
	
	while (1) {
		int ret;
		do {
			ret = poll(pfd, 1, 1000);
		} while (ret < 0 && errno == EINTR);
		
		if (ret < 0) {
			perror("[io bridge] poll");
			return -1;
		}
		
		if ((pfd[0].revents & (POLLERR | POLLHUP))) {
			perror("[io bridge] poll");
			return -1;
		}
		
		if ((pfd[0].revents & POLLIN)) {
			int new_client = accept(sock, NULL, NULL);
			if (new_client < 0) {
				perror("[io bridge] accept");
				return -1;
			}
			
			return new_client;
		}
	}
	
	return -1;
}

static int _async_write_chunk(int sock, uint8_t *data, int size) {
	struct pollfd pfd[1] = {
		{.fd = sock, .events = POLLOUT}
	};
	
	int ret;
	do {
		ret = poll(pfd, 1, 1000);
	} while (ret < 0 && errno == EINTR);
	
	if (ret < 0) {
		perror("[io bridge] poll");
		return -1;
	}
	
	if ((pfd[0].revents & (POLLERR | POLLHUP))) {
		perror("[io bridge] poll");
		return -1;
	}
	
	if ((pfd[0].revents & POLLOUT)) {
		ret = send(sock, data, size, 0);
		
		if (ret < 0) {
			perror("[io bridge] send");
			return -1;
		}
		
		return ret;
	}
	
	return 0;
}

static int _async_read_chunk(int sock, uint8_t *data, int size) {
	struct pollfd pfd[1] = {
		{.fd = sock, .events = POLLIN}
	};
	
	int ret;
	do {
		ret = poll(pfd, 1, 1000);
	} while (ret < 0 && errno == EINTR);
	
	if (ret < 0) {
		perror("[io bridge] poll");
		return -1;
	}
	
	if ((pfd[0].revents & (POLLERR | POLLHUP))) {
		perror("[io bridge] poll");
		return -1;
	}
	
	if ((pfd[0].revents & POLLIN)) {
		ret = recv(sock, data, size, 0);
		
		if (ret < 0) {
			perror("[io bridge] send");
			return -1;
		}
		
		return ret;
	}
	
	return 0;
}

static void _async_write(int sock, void *data, int size, int64_t timeout) {
	int written = 0;
	int64_t start = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
	do {
		if (timeout && qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - start > timeout) {
			fprintf(stderr, "[io bridge] timeout\r\n");
			exit(1);
		}
		
		int ret = _async_write_chunk(sock, (uint8_t *) (data + written), size - written);
		if (ret < 0) {
			fprintf(stderr, "[io bridge] IO error\r\n");
			exit(1);
		}
		
		written += ret;
	} while (written < size);
}

static void _async_read(int sock, void *data, int size, int64_t timeout) {
	int readed = 0;
	int64_t start = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
	do {
		if (timeout && qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - start > timeout) {
			fprintf(stderr, "[io bridge] timeout\r\n");
			exit(1);
		}
		
		int ret = _async_read_chunk(sock, (uint8_t *) (data + readed), size - readed);
		if (ret < 0) {
			fprintf(stderr, "[io bridge] IO error\r\n");
			exit(1);
		}
		
		readed += ret;
	} while (readed < size);
}
#endif
