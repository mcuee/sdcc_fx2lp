#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libusb-1.0/libusb.h>
#include <time.h>
#include <unistd.h>
#include <stdbool.h>

static libusb_context *ctx = NULL;
static libusb_device_handle *devh = NULL;

static volatile uint64_t total_bytes = 0;
static volatile uint64_t total_errors = 0;
static volatile uint64_t total_packets = 0;

static time_t start_time = 0;

void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -v <VID>          Vendor ID (hex, default: 0x04b4)\n");
    printf("  -p <PID>          Product ID (hex, default: 0x8613)\n");
    printf("  -s <size>         Transfer size in bytes (default: 262144)\n");
    printf("  -n <num>          Number of simultaneous URBs (default: 8)\n");
    printf("  -t <seconds>      Test duration in seconds (default: 10)\n");
    printf("  -e                Enable data integrity verification\n");
    printf("\nExample:\n");
    printf("  sudo %s -v 04b4 -p 8613 -s 1048576 -n 16 -t 15 -e\n", prog);
}

static void verify_pattern(const uint8_t *buf, int len)
{
    for (int i = 0; i < len; i++) {
        if (buf[i] != (i & 0xFF)) {
            total_errors++;
            return;
        }
    }
    total_packets++;
}

static void bulk_callback(struct libusb_transfer *transfer)
{
    if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        total_bytes += transfer->actual_length;
        if (transfer->user_data != NULL) {
            verify_pattern(transfer->buffer, transfer->actual_length);
        }
    } else if (transfer->status != LIBUSB_TRANSFER_CANCELLED) {
        fprintf(stderr, "Transfer error: %s\n", libusb_error_name(transfer->status));
    }

    libusb_submit_transfer(transfer);   // Keep the pipe full
}

int main(int argc, char **argv)
{
    int r;
    uint16_t vid = 0x04b4;
    uint16_t pid = 0x8613;
    size_t xfer_size = 256 * 1024;
    int num_urbs = 8;
    double test_duration = 10.0;
    bool verify = false;

    uint8_t *buffer = NULL;
    struct libusb_transfer **transfers = NULL;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 && i+1 < argc) vid = (uint16_t)strtol(argv[++i], NULL, 16);
        else if (strcmp(argv[i], "-p") == 0 && i+1 < argc) pid = (uint16_t)strtol(argv[++i], NULL, 16);
        else if (strcmp(argv[i], "-s") == 0 && i+1 < argc) xfer_size = (size_t)atol(argv[++i]);
        else if (strcmp(argv[i], "-n") == 0 && i+1 < argc) num_urbs = atoi(argv[++i]);
        else if (strcmp(argv[i], "-t") == 0 && i+1 < argc) test_duration = atof(argv[++i]);
        else if (strcmp(argv[i], "-e") == 0) verify = true;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    printf("=== Bulk Loopback Performance Tester ===\n");
    printf("VID:PID = 0x%04x:0x%04x | Size: %zu B | URBs: %d | Duration: %.1fs | Verify: %s\n\n",
           vid, pid, xfer_size, num_urbs, test_duration, verify ? "ON" : "OFF");

    r = libusb_init(&ctx);
    if (r < 0) {
        fprintf(stderr, "libusb_init failed: %s\n", libusb_error_name(r));
        return 1;
    }

    devh = libusb_open_device_with_vid_pid(ctx, vid, pid);
    if (!devh) {
        fprintf(stderr, "Device 0x%04x:0x%04x not found.\n", vid, pid);
        goto cleanup;
    }

    libusb_set_auto_detach_kernel_driver(devh, 1);
    r = libusb_claim_interface(devh, 0);
    if (r < 0) {
        fprintf(stderr, "Claim interface failed: %s\n", libusb_error_name(r));
        goto cleanup;
    }

    // Allocate resources
    buffer = malloc(xfer_size);
    if (!buffer) {
        fprintf(stderr, "Buffer allocation failed\n");
        goto cleanup;
    }

    for (size_t i = 0; i < xfer_size; i++)
        buffer[i] = i & 0xFF;

    transfers = calloc(num_urbs * 2, sizeof(struct libusb_transfer*));
    if (!transfers) {
        fprintf(stderr, "Transfers array allocation failed\n");
        goto cleanup;
    }

    start_time = time(NULL);

    // Submit transfers (OUT + IN)
    for (int i = 0; i < num_urbs; i++) {
        // OUT
        transfers[i] = libusb_alloc_transfer(0);
        libusb_fill_bulk_transfer(transfers[i], devh, 0x02,
                                  buffer, xfer_size, bulk_callback,
                                  verify ? (void*)1 : NULL, 0);
        libusb_submit_transfer(transfers[i]);

        // IN
        transfers[num_urbs + i] = libusb_alloc_transfer(0);
        libusb_fill_bulk_transfer(transfers[num_urbs + i], devh, 0x86,
                                  buffer, xfer_size, bulk_callback,
                                  verify ? (void*)1 : NULL, 0);
        libusb_submit_transfer(transfers[num_urbs + i]);
    }

    printf("Test started. Press Ctrl+C to stop early.\n");

    while (difftime(time(NULL), start_time) < test_duration) {
        libusb_handle_events_timeout(ctx, &(struct timeval){0, 50000});

        if (time(NULL) % 2 == 0) {
            double elapsed = difftime(time(NULL), start_time);
            double mbps = (total_bytes / (1024.0 * 1024.0)) / elapsed;
            printf("[%5.1fs] Throughput: %7.2f MB/s | Errors: %llu\n", elapsed, mbps, total_errors);
        }
    }

    printf("\n=== FINAL RESULTS ===\n");
    double elapsed = difftime(time(NULL), start_time);
    printf("Total bytes transferred : %llu\n", total_bytes);
    printf("Average throughput      : %.2f MB/s\n", (total_bytes / (1024.0 * 1024.0)) / elapsed);
    if (verify)
        printf("Data verification       : %llu packets, %llu errors\n", total_packets, total_errors);

cleanup:
    if (transfers) {
        for (int i = 0; i < num_urbs * 2; i++) {
            if (transfers[i]) {
                libusb_cancel_transfer(transfers[i]);
                libusb_free_transfer(transfers[i]);
            }
        }
        free(transfers);
    }

    free(buffer);

    if (devh) {
        libusb_release_interface(devh, 0);
        libusb_close(devh);
    }
    if (ctx) libusb_exit(ctx);

    return 0;
}