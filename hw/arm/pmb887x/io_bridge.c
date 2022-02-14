#include <errno.h>
#include <stdio.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h> 
#include <sys/un.h>
#include <stdlib.h>
#include <stdint.h>

#include "hw/arm/pmb887x/io_bridge.h"

#include "hw/qdev-core.h"
#include "hw/irq.h"
#include "qemu/thread.h"
#include "qemu/main-loop.h"
#include "cpu.h"
#include "sysemu/cpu-timers.h"

#include "hw/arm/pmb887x/regs.h"

static int sock_server_io = -1;
static int sock_server_irq = -1;

static int sock_client_io = -1;
static int sock_client_irq = -1;

static char cmd_w_size[] = {0, 'o', 'w', 'O', 'W'};
static char cmd_r_size[] = {0, 'i', 'r', 'I', 'R'};

static int _open_unix_sock(const char *name);
static int _wait_for_client(int sock);
static void _async_read(int sock, void *data, int size);
static void _async_write(int sock, void *data, int size);
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
		_async_read(sock_client_irq, &irq, 1);
		
		bool locked = qemu_mutex_iothread_locked();
		if (!locked)
			qemu_mutex_lock_iothread();
		
		if (irq) {
			qemu_set_irq(qdev_get_gpio_in(nvic, irq), 100000);
			current_irq = irq;
		}
		
		if (!locked)
			qemu_mutex_unlock_iothread();
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
	
	_async_write(sock_client_io, &cmd_r_size[size], 1);
	_async_write(sock_client_io, &addr, 4);
	_async_write(sock_client_io, &from, 4);
	
	uint8_t buf[5];
	_async_read(sock_client_io, &buf, 5);
	
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
	
	_async_write(sock_client_io, &cmd_w_size[size], 1);
	_async_write(sock_client_io, &addr, 4);
	_async_write(sock_client_io, &value, 4);
	_async_write(sock_client_io, &from, 4);
	
	uint8_t buf;
	_async_read(sock_client_io, &buf, 1);
	
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

static void _async_write(int sock, void *data, int size) {
	int written = 0;
	do {
		int ret = _async_write_chunk(sock, (uint8_t *) (data + written), size - written);
		if (ret < 0) {
			fprintf(stderr, "[io bridge] IO error\r\n");
			exit(1);
		}
		
		written += ret;
	} while (written < size);
}

static void _async_read(int sock, void *data, int size) {
	int readed = 0;
	do {
		int ret = _async_read_chunk(sock, (uint8_t *) (data + readed), size - readed);
		if (ret < 0) {
			fprintf(stderr, "[io bridge] IO error\r\n");
			exit(1);
		}
		
		readed += ret;
	} while (readed < size);
}

/*

static FILE *bridge_fp = NULL;

static char cmd_w_size[] = {0, 'o', 'w', 'O', 'W'};
static char cmd_r_size[] = {0, 'i', 'r', 'I', 'R'};

static FILE *sie_bridge_fp(void) {
	if (!bridge_fp)
		bridge_fp = fopen("/dev/shm/pmb8876_io_bridge.sock", "w+");
	return bridge_fp;
}

void pmb8876_io_bridge_write(unsigned int addr, unsigned int size, unsigned int value, unsigned int from) {
	if (from % 4 == 0)
		from -= 4;
	else
		from -= 2;
	
	FILE *fp = sie_bridge_fp();
	while (ftruncate(fileno(fp), 0) == 0) break;
	fseek(fp, 0, SEEK_SET);
	fwrite(&cmd_w_size[size], 1, 1, fp);
	fwrite(&addr, 4, 1, fp);
	fwrite(&value, 4, 1, fp);
	fwrite(&from, 4, 1, fp);
	
	int tell;
	unsigned char buf;
	while (1) {
		fseek(fp, 0, SEEK_END);
		tell = ftell(fp);
		if (tell == 1 || tell == 2) {
			fseek(fp, 0, SEEK_SET);
			fflush(fp);
			if (fread(&buf, 1, 1, fp) == 1) {
				if (buf == 0x2B) { // IRQ
					if (fread(&buf, 1, 1, fp) == 1) {
					//	fprintf(stderr, "**** NEW IRQ: %02X\n", buf);
						pmb8876_trigger_irq(buf);
					} else {
						fprintf(stderr, "%s(%08X, %08X, %08X): Read IRQ error\n", __func__, addr, value, from);
						exit(1);
					}
				} else if (buf != 0x21) {
					fprintf(stderr, "%s(%08X, %08X, %08X): Invalid ACK: 0x%02X\n", __func__, addr, value, from, buf);
					exit(1);
				}
				break;
			}
		}
	}
}

unsigned int pmb8876_io_bridge_read(unsigned int addr, unsigned int size, unsigned int from) {
	if (from % 4 == 0)
		from -= 4;
	else
		from -= 2;
	
	FILE *fp = sie_bridge_fp();
	while (ftruncate(fileno(fp), 0) == 0) break;
	fseek(fp, 0, SEEK_SET);
	fwrite(&cmd_r_size[size], 1, 1, fp);
	fwrite(&addr, 4, 1, fp);
	fwrite(&from, 4, 1, fp);
	
	int tell;
	unsigned char irq;
	unsigned char buf[5];
	while (1) {
		fseek(fp, 0, SEEK_END);
		tell = ftell(fp);
		if (tell == 5 || tell == 6) {
			fseek(fp, 0, SEEK_SET);
			fflush(fp);
			if (fread(buf, 5, 1, fp) == 1) {
				if (buf[0] == 0x2B) { // IRQ
					if (fread(&irq, 1, 1, fp) == 1) {
					//	fprintf(stderr, "**** NEW IRQ: %02X\n", irq);
						pmb8876_trigger_irq(irq);
					} else {
						fprintf(stderr, "%s(%08X, %08X): Read IRQ error\n", __func__, addr, from);
						exit(1);
					}
				} else if (buf[0] != 0x21) {
					fprintf(stderr, "%s(%08X, %08X): Invalid ACK: 0x%02X\n", __func__, addr, from, buf[0]);
					exit(1);
				}
				return buf[4] << 24 | buf[3] << 16 | buf[2] << 8 | buf[1];
			}
		}
	}
}
* */
