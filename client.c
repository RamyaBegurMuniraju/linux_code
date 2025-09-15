#include <sys/time.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <sys/ioctl.h>
#include <bluetooth/l2cap.h>

#define PSM 0x25  // Must match the server's PSM
#define TARGET_ADDR "28:CD:C1:12:E3:BA" 

#define TOTAL_SIZE (2 * 1024 * 1024)  // 2MB
#define CHUNK_SIZE 512               // Sending in 512 byte chunks


int send_conn_update(int hci_sock, uint16_t handle) {
	struct {
		uint16_t handle;
		uint16_t interval_min;
		uint16_t interval_max;
		uint16_t latency;
		uint16_t supervision_timeout;
		uint16_t min_ce_length;
		uint16_t max_ce_length;
	} __attribute__((packed)) cp;

	cp.handle = htobs(handle);
	cp.interval_min = htobs(0x000C);  // 15ms
	cp.interval_max = htobs(0x000C);  // 15ms
	cp.latency = htobs(0x0000);
	cp.supervision_timeout = htobs(0x01F4);  // 5s
	cp.min_ce_length = htobs(0x0000);
	cp.max_ce_length = htobs(0x0000);

	return hci_send_cmd(hci_sock, OGF_LE_CTL, OCF_LE_CONN_UPDATE,
			sizeof(cp), &cp);
}


// Given bdaddr_t bdaddr (the target)
int get_conn_handle(int hci_sock, const bdaddr_t *bdaddr) {
	struct hci_conn_info_req *req;
	req = malloc(sizeof(struct hci_conn_info_req) + sizeof(struct hci_conn_info));
	bacpy(&req->bdaddr, bdaddr);
	req->type = ACL_LINK;

	if (ioctl(hci_sock, HCIGETCONNINFO, (unsigned long)req) < 0) {
		perror("GetConnInfo");
		char debug_addr[18];
		ba2str(bdaddr, debug_addr);
		printf("Failed for address: %s\n", debug_addr);
		free(req);
		return -1;
	}

	uint16_t handle = req->conn_info->handle;
	free(req);
	return handle;
}




struct le_set_phy_cp {
	uint16_t handle;
	uint8_t tx_phys;
	uint8_t rx_phys;
	uint8_t phy_opts;
} __attribute__((packed));

void set_2m_phy(int sock, uint16_t handle) {
	struct le_set_phy_cp cp;
	memset(&cp, 0, sizeof(cp));

	cp.handle = htobs(handle);
	cp.tx_phys = 0x02;   // Request 2M PHY for TX
	cp.rx_phys = 0x02;   // Request 2M PHY for RX
	cp.phy_opts = 0x00;  // No special options

	struct hci_request rq;
	memset(&rq, 0, sizeof(rq));
	rq.ogf = OGF_LE_CTL;
	rq.ocf = 0x0032; // LE Set PHY
	rq.cparam = &cp;
	rq.clen = sizeof(cp);
	rq.rparam = NULL;
	rq.rlen = 0;

	if (hci_send_req(sock, &rq, 1000) < 0) {
		perror("Failed to set PHY");
	} else {
		printf("Requested 2M PHY\n");
	}
}




int main() {
	uint8_t buf[CHUNK_SIZE];
	ssize_t bytes_written;
	size_t total_sent = 0;

	struct timeval t_start, t_end;

	int sock;
	struct sockaddr_l2 addr;

	// Fill buffer with dummy data
	memset(buf, 'A', sizeof(buf));


	// Create L2CAP socket
	sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
	if (sock < 0) {
		perror("socket");
		return 1;
	}

	// Setup destination address
	memset(&addr, 0, sizeof(addr));
	addr.l2_family = AF_BLUETOOTH;
	addr.l2_psm = htobs(PSM);
	addr.l2_bdaddr_type = BDADDR_LE_PUBLIC;  // or BDADDR_LE_RANDOM depending on your Pico W
	str2ba(TARGET_ADDR, &addr.l2_bdaddr);



	// Setup sockaddr_l2 and connect as usual
	struct l2cap_options opts;
	socklen_t optlen = sizeof(opts);

#if 1
	if (getsockopt(sock, SOL_L2CAP, L2CAP_OPTIONS, &opts, &optlen) == 0) {
		opts.omtu = 247;  // outgoing MTU
		opts.imtu = 247;  // incoming MTU
		if (setsockopt(sock, SOL_L2CAP, L2CAP_OPTIONS, &opts, sizeof(opts)) < 0) {
			perror("setsockopt L2CAP_OPTIONS");
			// handle error
		}
	}	

#endif
	// Connect to server
	printf("Connecting to %s on PSM 0x%02x...\n", TARGET_ADDR, PSM);
	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("connect");
		close(sock);
		return 1;
	}
	sleep(2);
	bdaddr_t bd_addr;
	str2ba(TARGET_ADDR, &bd_addr);
	int dev_id = hci_get_route(&bd_addr); // returns matching adapter index
	int hci_sock = hci_open_dev(dev_id); // or get adapter index dynamically


	printf("Connected! Sending 2MB of data...\n");


	sleep(10);
	gettimeofday(&t_start, NULL);

	while (total_sent < TOTAL_SIZE) {
		size_t to_send = CHUNK_SIZE;
		if (TOTAL_SIZE - total_sent < CHUNK_SIZE) {
			to_send = TOTAL_SIZE - total_sent;
		}

		bytes_written = write(sock, buf, to_send);
		if (bytes_written <= 0) {
			perror("Write failed");
			break;
		}

		total_sent += bytes_written;

		// Optional: print progress
		if (total_sent % (256 * 1024) == 0) {  // Every 256 KB
			printf("Sent %lu / %d bytes (%.1f%%)\n",
					total_sent, TOTAL_SIZE,
					100.0 * total_sent / TOTAL_SIZE);
		}
	}

	gettimeofday(&t_end, NULL);

	double seconds = (t_end.tv_sec - t_start.tv_sec) +
		(t_end.tv_usec - t_start.tv_usec) / 1000000.0;

	double kBps = (total_sent / 1024.0) / seconds;

	printf("\nSent %.2f MB in %.2f seconds → %.2f kB/s\n",
			total_sent / (1024.0 * 1024.0), seconds, kBps);

	close(sock);
	return 0;
}

