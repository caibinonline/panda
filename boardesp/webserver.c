#include "stdlib.h"
#include "ets_sys.h"
#include "osapi.h"
#include "gpio.h"
#include "mem.h"
#include "os_type.h"
#include "user_interface.h"
#include "espconn.h"
#include "upgrade.h"

#include "crypto/rsa.h"
#include "crypto/sha.h"

#include "gitversion.h"
#include "cert.h"

#define max(a,b) ((a) > (b) ? (a) : (b))
#define min(a,b) ((a) < (b) ? (a) : (b))
#define espconn_send_string(conn, x) espconn_send(conn, x, strlen(x))

char resp[0x800];
char staticpage[] = "HTTP/1.0 200 OK\nContent-Type: text/html\n\n"
"<pre>This is your comma.ai panda<br/><br/>"
"It's open source. Find the code <a href=\"https://github.com/commaai/panda\">here</a><br/>"
"Designed to work with our dashcam, <a href=\"http://chffr.comma.ai\">chffr</a><br/>";

static struct espconn web_conn;
static esp_tcp web_proto;
extern char ssid[];

char *st_firmware;
int real_content_length, content_length = 0;
char *st_firmware_ptr;
LOCAL os_timer_t ota_reboot_timer;

#define FIRMWARE_SIZE 503808

void st_flash() {
  int i;

  // stupid watchdog test
  //system_soft_wdt_stop();

  // boot mode
  st_set_boot_mode(1);

  // unlock flash
  st_cmd(0x10, 0, NULL);
  st_cmd(0xf, 0, NULL);

  // erase sector 1
  st_cmd(0x11, 1, NULL);
  st_cmd(0xf, 0, NULL);

  if (real_content_length >= 16384) {
    // erase sector 2
    st_cmd(0x11, 2, NULL);
    st_cmd(0xf, 0, NULL);
  }

  // real content length will always be 0x10 aligned
  for (i = 0; i < real_content_length; i += 0x10) {
    st_cmd(0x12, 4, &st_firmware[i]);
    system_soft_wdt_feed();
  }

  // reboot into normal mode
  st_set_boot_mode(0);

  // done with this
  os_free(st_firmware);

  // watchdog done
  //system_soft_wdt_restart();
}

typedef enum {
    NOT_STARTED,
    CONNECTION_ESTABLISHED,
    RECEIVING_HEADER,
    RECEIVING_ST_FIRMWARE,
    RECEIVING_ESP_FIRMWARE,
    REBOOTING,
    ERROR
} web_state_t;

web_state_t state = NOT_STARTED;
int esp_address, esp_address_erase_limit, start_address;

void hexdump(char *data, int len) {
  int i;
  for (i=0;i<len;i++) {
    if (i!=0 && (i%0x10)==0) os_printf("\n");
    os_printf("%02X ", data[i]);
  }
  os_printf("\n");
}

void st_reset() {
  // reset the ST
  gpio16_output_conf();
  gpio16_output_set(0);
  os_delay_us(1000);
  gpio16_output_set(1);
  os_delay_us(10000);
}

void st_set_boot_mode(int boot_mode) {
  if (boot_mode) {
    // boot mode (pull low)
    gpio_output_set(0, (1 << 4), (1 << 4), 0);
    st_reset();
  } else {
    // no boot mode (pull high)
    gpio_output_set((1 << 4), 0, (1 << 4), 0);
    st_reset();
  }
}

int st_cmd(int d1, int d2, char *data) {
  uint32_t __dat[0x14];
  char *dat = (char *)__dat;
  memset(dat, 0, 0x14);
  uint32_t recv[0x44/4];
  dat[0] = d1;
  dat[1] = 0xFF^d1;
  dat[2] = d2;
  dat[3] = 0xFF^d2;
  if (data != NULL) memcpy(dat+4, data, 0x10);
  hexdump(dat, 0x14);

  spi_comm(dat, 0x14, recv, 0x40);
  return memcmp(recv+0x10, "\xde\xad\xd0\x0d", 4)==0;
}

static void ICACHE_FLASH_ATTR web_rx_cb(void *arg, char *data, uint16_t len) {
  int i;
  struct espconn *conn = (struct espconn *)arg;
  if (state == CONNECTION_ESTABLISHED) {
    state = RECEIVING_HEADER;
    os_printf("%s %d\n", data, len);

    // index
    if (memcmp(data, "GET / ", 6) == 0) {
      strcpy(resp, staticpage);
      ets_strcat(resp, "<br/>ssid: ");
      ets_strcat(resp, ssid);
      ets_strcat(resp, "<br/>");

      ets_strcat(resp, "<br/>st version:     ");
      uint32_t recvData[0x11];
      int len = spi_comm("\x00\x00\x00\x00\x40\xD6\x00\x00\x00\x00\x40\x00", 0xC, recvData, 0x40);
      ets_memcpy(resp+strlen(resp), recvData+1, len);

      ets_strcat(resp, "<br/>esp version:    ");
      ets_strcat(resp, gitversion);
      uint8_t current = system_upgrade_userbin_check();
      if (current == UPGRADE_FW_BIN1) {
        ets_strcat(resp, "<br/>esp flash file: user2.bin");
      } else {
        ets_strcat(resp, "<br/>esp flash file: user1.bin");
      }
      espconn_send_string(&web_conn, resp);
      espconn_disconnect(conn);
    } else if (memcmp(data, "PUT /stupdate ", 14) == 0) {
      os_printf("init st firmware\n");
      char *cl = strstr(data, "Content-Length: ");
      if (cl != NULL) {
        // get content length
        cl += strlen("Content-Length: ");
        content_length = skip_atoi(&cl);
        os_printf("with content length %d\n", content_length);

        // should be small enough to fit in RAM
        real_content_length = (content_length+0xF)&(~0xF);
        st_firmware_ptr = st_firmware = os_malloc(real_content_length);
        memset(st_firmware, 0, real_content_length);
        state = RECEIVING_ST_FIRMWARE;
      }
    } else if ((memcmp(data, "PUT /espupdate1 ", 16) == 0) ||
               (memcmp(data, "PUT /espupdate2 ", 16) == 0)) {
      // 0x1000   = user1.bin
      // 0x81000  = user2.bin
      // 0x3FE000 = blank.bin
      os_printf("init st firmware\n");
      char *cl = strstr(data, "Content-Length: ");
      if (cl != NULL) {
        // get content length
        cl += strlen("Content-Length: ");
        content_length = skip_atoi(&cl);
        os_printf("with content length %d\n", content_length);

        // setup flashing
        uint8_t current = system_upgrade_userbin_check();
        if (data[14] == '2' && current == UPGRADE_FW_BIN1) {
          os_printf("flashing boot2.bin\n");
          state = RECEIVING_ESP_FIRMWARE;
          esp_address = 4*1024 + FIRMWARE_SIZE + 16*1024 + 4*1024;
        } else if (data[14] == '1' && current == UPGRADE_FW_BIN2) {
          os_printf("flashing boot1.bin\n");
          state = RECEIVING_ESP_FIRMWARE;
          esp_address = 4*1024;
        } else {
          espconn_send_string(&web_conn, "HTTP/1.0 404 Not Found\nContent-Type: text/html\n\nwrong!\n");
          espconn_disconnect(conn);
        }
        esp_address_erase_limit = esp_address;
        start_address = esp_address;
      }
    } else {
      espconn_disconnect(conn);
    }
  } else if (state == RECEIVING_ST_FIRMWARE) {
    os_printf("receiving st firmware: %d/%d\n", len, content_length);
    memcpy(st_firmware_ptr, data, min(content_length, len));
    st_firmware_ptr += len;
    content_length -= len;

    if (content_length <= 0 && real_content_length > 1000) {
      state = NOT_STARTED;
      os_printf("done!\n");
      espconn_send_string(&web_conn, "HTTP/1.0 200 OK\nContent-Type: text/html\n\nflashing...\n");
      espconn_disconnect(conn);

      // reboot
      os_printf("Scheduling st_flash in 100ms.\n");
      os_timer_disarm(&ota_reboot_timer);
      os_timer_setfn(&ota_reboot_timer, (os_timer_func_t *)st_flash, NULL);
      os_timer_arm(&ota_reboot_timer, 100, 0);
    }
  } else if (state == RECEIVING_ESP_FIRMWARE) {
    if ((esp_address+len) < (start_address + FIRMWARE_SIZE)) {
      os_printf("receiving esp firmware: %d/%d -- 0x%x - 0x%x\n", len, content_length,
        esp_address, esp_address_erase_limit);
      content_length -= len;
      while (esp_address_erase_limit < (esp_address + len)) {
        os_printf("erasing 0x%X\n", esp_address_erase_limit);
        spi_flash_erase_sector(esp_address_erase_limit / SPI_FLASH_SEC_SIZE);
        esp_address_erase_limit += SPI_FLASH_SEC_SIZE;
      }
      SpiFlashOpResult res = spi_flash_write(esp_address, data, len);
      if (res != SPI_FLASH_RESULT_OK) {
        os_printf("flash fail @ 0x%x\n", esp_address);
      }
      esp_address += len;

      if (content_length == 0) {

        char digest[SHA_DIGEST_SIZE];
        uint32_t rsa[RSANUMBYTES/4];
        uint32_t dat[0x80/4];
        int ll;
        spi_flash_read(esp_address-RSANUMBYTES, rsa, RSANUMBYTES);

        // 32-bit aligned accesses only
        SHA_CTX ctx;
        SHA_init(&ctx);
        for (ll = start_address; ll < esp_address-RSANUMBYTES; ll += 0x80) {
          spi_flash_read(ll, dat, 0x80);
          SHA_update(&ctx, dat, min((esp_address-RSANUMBYTES)-ll, 0x80));
        }
        memcpy(digest, SHA_final(&ctx), SHA_DIGEST_SIZE);

        if (RSA_verify(&releaseesp_rsa_key, rsa, RSANUMBYTES, digest, SHA_DIGEST_SIZE) ||
          #ifdef ALLOW_DEBUG
            RSA_verify(&debugesp_rsa_key, rsa, RSANUMBYTES, digest, SHA_DIGEST_SIZE)
          #else
            false
          #endif
          ) {
          os_printf("RSA verify success!\n");
          espconn_send_string(&web_conn, "HTTP/1.0 200 OK\nContent-Type: text/html\n\nsuccess!\n");
          system_upgrade_flag_set(UPGRADE_FLAG_FINISH);

          // reboot
          os_printf("Scheduling reboot.\n");
          os_timer_disarm(&ota_reboot_timer);
          os_timer_setfn(&ota_reboot_timer, (os_timer_func_t *)system_upgrade_reboot, NULL);
          os_timer_arm(&ota_reboot_timer, 2000, 0);
        } else {
          os_printf("RSA verify FAILURE\n");
          espconn_send_string(&web_conn, "HTTP/1.0 500 Internal Server Error\nContent-Type: text/html\n\nrsa verify fail\n");
        }
        espconn_disconnect(conn);
      }
    }
  }
}

void ICACHE_FLASH_ATTR web_tcp_connect_cb(void *arg) {
  state = CONNECTION_ESTABLISHED;
  struct espconn *conn = (struct espconn *)arg;
  espconn_set_opt(&web_conn, ESPCONN_NODELAY);
  espconn_regist_recvcb(conn, web_rx_cb);
}

void ICACHE_FLASH_ATTR web_init() {
  web_proto.local_port = 80;
  web_conn.type = ESPCONN_TCP;
  web_conn.state = ESPCONN_NONE;
  web_conn.proto.tcp = &web_proto;
  espconn_regist_connectcb(&web_conn, web_tcp_connect_cb);
  espconn_accept(&web_conn);
}

