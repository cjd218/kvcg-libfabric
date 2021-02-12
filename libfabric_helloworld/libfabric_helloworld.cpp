/*
 ***********************************************************
 * libfabric hello world
 *
 * compile: gcc libfabric_helloworld.cpp -lstdc++ -lfabric
 *
 * Can debug libfabric by exporting FI_LOG_LEVEL=debug
 * in environment before executing.
 ***********************************************************
 */

// standard includes
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>
#include <errno.h>

// libfabric includes
#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_tagged.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_errno.h>

static uint8_t *local_buf;
static uint8_t *remote_buf;
static struct fi_info *fi, *hints;
static struct fid_fabric *fabric;
static struct fi_eq_attr eq_attr;
static struct fid_eq *eq;
static struct fid_domain *domain;
static struct fid_ep *ep;
static struct fi_av_attr av_attr;
static struct fid_av *av;
static struct fi_cq_attr cq_attr;
static struct fid_cq *tx_cq, *rx_cq;
static struct fid_mr *mr;
static fi_addr_t remote_addr;
static void* addr = NULL; // Will contain endpoint address
static size_t addrlen = 0;
static size_t max_msg_size = 4096;

static char* dst_addr; // client specifies server

/* Wait for a new completion on the completion queue */
int wait_for_completion(struct fid_cq *cq) {
    struct fi_cq_entry entry;
    int ret;
    while(1) {
        ret = fi_cq_read(cq, &entry, 1);
        if (ret > 0) return 0;
        if (ret != -FI_EAGAIN) {
            // New error on queue
            struct fi_cq_err_entry err_entry;
            fi_cq_readerr(cq, &err_entry, 0);
            std::cout << fi_strerror(err_entry.err) << " " <<
                         fi_cq_strerror(cq, err_entry.prov_errno,
                                        err_entry.err_data, NULL, 0) << std::endl;
            return ret;
        }
    }
}

int init_fabric() {
    int err = 0; // specific libfabric error
    int ret = 0;

    // Get list of providers (verbs, psm2, tcp).
    // Can specify 'hints' to limit them. For now, skip hints.
    std::cout << "Getting fi provider" << std::endl;
    hints = fi_allocinfo();
    hints->caps = FI_MSG;
    hints->ep_attr->type = FI_EP_RDM;
    if (dst_addr) {
        /* client */
        err = fi_getinfo(FI_VERSION(1, 7), dst_addr, "4092", 0, hints, &fi);
    } else {
        /* server */
        err = fi_getinfo(FI_VERSION(1, 7), NULL, "4092", FI_SOURCE, hints, &fi);
    }
    if (err)
        goto exit;

    // fi is a linked list of providers. For now just use first one
    std::cout << "Using provider: " << fi->fabric_attr->prov_name << std::endl;

    // Create a fabric object. This is the parent of everything else.
    std::cout << "Creating fabric object" << std::endl;
    err = fi_fabric(fi->fabric_attr, &fabric, NULL);
    if (err)
        goto exit;

    // Create a domain. This represents logical connection into fabric.
    // For example, this may map to a physical NIC, and defines the boundary
    // four fabric resources. Most other objects belong to a domain.
    std::cout << "Creating domain" << std::endl;
    err = fi_domain(fabric, fi, &domain, NULL);
    if (err)
        goto exit;

    // Create a completion queue. This reports data transfer operation completions.
    // Unlike fabric event queues, these are associated with a single hardware NIC.
    // Will create one for tx and one for rx
    std::cout << "Creating tx completion queue" << std::endl;
    memset(&cq_attr, 0, sizeof(cq_attr));
    cq_attr.wait_obj = FI_WAIT_NONE;
    //cq_attr.format = FI_CQ_FORMAT_CONTEXT;
    cq_attr.size = fi->tx_attr->size;
    err = fi_cq_open(domain, &cq_attr, &tx_cq, NULL);
    if (err)
        goto exit;
    std::cout << "Creating rx completion queue" << std::endl;
    cq_attr.size = fi->rx_attr->size;
    err = fi_cq_open(domain, &cq_attr, &rx_cq, NULL);
    if (err)
        goto exit;

    // Create an address vector. This allows connectionless endpoints to communicate
    // without having to resolve addresses, such as IPv4, during data transfers.
    std::cout << "Creating address vector" << std::endl;
    memset(&av_attr, 0, sizeof(av_attr));
    av_attr.type = fi->domain_attr->av_type ?
        fi->domain_attr->av_type : FI_AV_MAP;
    av_attr.count = 1;
    av_attr.name = NULL;
    err = fi_av_open(domain, &av_attr, &av, NULL);
    if (err)
        goto exit;

exit:
    if (err) {
        std::cerr << "ERROR (" << err << "): " << fi_strerror(-err) << std::endl;
        ret = err;
    }

    return ret;
}

int init_endpoint() {
    int ret = 0;
    int err = 0;

    // Create endpoints, the object used for communication. Typically associated with
    // a single hardware NIC, they are conceptually similar to a socket.
    std::cout << "Creating endpoint" << std::endl;
    err = fi_endpoint(domain, fi, &ep, NULL);
    if (err)
        goto exit;
    // Could create multiple endpoints, especially if there are multiple NICs available.

    // malloc buffers
    local_buf = (uint8_t*) malloc(max_msg_size);
    remote_buf = (uint8_t*) malloc(max_msg_size);
    if (!local_buf || ! remote_buf) {
        std::cerr << "malloc failure" << std::endl;
        ret = -1;
        goto exit;
    }
    memset(local_buf, 0, max_msg_size);
    memset(remote_buf, 0, max_msg_size);


exit:
    if (err) {
        std::cerr << "ERROR (" << err << "): " << fi_strerror(-err) << std::endl;
        ret = err;
    }

    return ret;
}

int bind_endpoint() {
    int ret = 0;
    int err = 0;

    // Bind AV to endpoint
    std::cout << "Binding AV to EP" << std::endl;
    err = fi_ep_bind(ep, &av->fid, 0);
    if (err)
        goto exit;

    // Bind Tx CQ
    std::cout << "Binding Tx CQ to EP" << std::endl;
    err = fi_ep_bind(ep, &tx_cq->fid, FI_TRANSMIT);
    if (err)
        goto exit;

    // Bind Rx CQ
    std::cout << "Binding Rx CQ to EP" << std::endl;
    err = fi_ep_bind(ep, &rx_cq->fid, FI_RECV);
    if (err)
        goto exit;

    // Enable EP
    std::cout << "Enabling EP" << std::endl;
    err = fi_enable(ep);
    if (err)
        goto exit;

    // Register memory region for RDMA
    std::cout << "Registering memory region" << std::endl;
    err = fi_mr_reg(domain, remote_buf, max_msg_size,
                    FI_WRITE|FI_REMOTE_WRITE|FI_READ|FI_REMOTE_READ, 0,
                    0, 0, &mr, NULL);
    if (err)
        goto exit;

    // Bind memory region to receive counter
    //std::cout << "Binding MR to Rx CQ" <<  std::endl;
    //err= fi_mr_bind(mr, &rx_cq->fid, FI_REMOTE_WRITE|FI_REMOTE_READ);
    //if (err)
    //    goto exit;

exit:
    if (err) {
        std::cerr << "ERROR (" << err << "): " << fi_strerror(-err) << std::endl;
        ret = err;
    }

    return ret;
}

int release_all() {
    // Cleanup when finished
    int ret = 0;
    std::cout << "Cleaning up" << std::endl;
    if (hints)
        fi_freeinfo(hints);
    if (fi)
        fi_freeinfo(fi);
#if 0
    if (av)
        ret |= fi_close(&av->fid);
    if (mr)
        ret |= fi_close(&mr->fid);
    if (tx_cq)
        ret |= fi_close(&tx_cq->fid);
    if (rx_cq)
        ret |= fi_close(&rx_cq->fid);
#endif
    if (local_buf)
        free(local_buf);
    if (remote_buf)
        free(remote_buf);
    if (addr)
        free(addr);

    if (ret) {
        std::cerr << "ERROR - cleanup was not successful" << std::endl;
    }
    return ret;
}

void usage() {
    std::cout << "Usage: ./libfabric_helloworld [optional server address]" << std::endl;
    std::cout << "            server address - remote server to connect to as a client." << std::endl;
    std::cout << "                             If not specified, will run as a server." << std::endl;
}

int main(int argc, char **argv) {
    int err = 0;
    int ret = 0;

    if (argc < 2) {
        // No args, run as server
        std::cout << "Running as SERVER" << std::endl;
    } else if (argc == 2) {
        if (argv[1] == "--help") {
            usage();
            return 0;
        } else {
            dst_addr = argv[1];
            std::cout <<  "Running as CLIENT - server addr=" << dst_addr << std::endl;
        }
    } else {
        std::cout << "Too many arguments!" << std::endl;
        usage();
        return -1;
    }


    // Step 1: Init fabric objects
    if(ret = init_fabric()) goto exit;
    // Step 2: Initialize endpoint
    if(ret = init_endpoint()) goto exit;
    // Step 3: Bind fabric to endpoint
    if(ret = bind_endpoint()) goto exit;

    fi_recv(ep, remote_buf, max_msg_size, 0, 0, NULL);

    if (dst_addr) {
        /* CLIENT */
        // Add server to AV
        std::cout << "Client: Adding server " << fi->dest_addr << " to AV" << std::endl;
        if(1 != fi_av_insert(av, fi->dest_addr, 1, &remote_addr, 0, NULL)) {
            // fi_av_insert returns number of successful adds. should be 1.
            std::cerr << "ERROR - fi_av_insert did not return 1" << std::endl;
            ret = -1;
            goto exit;
        } else if (errno) {
            std::cerr << "ERROR - " << errno << std::endl;
        }

        if(fi->domain_attr->av_type == FI_AV_TABLE) {
            std::cerr << "ERROR - can not support FI_AV_TABLE" << std::endl;
            ret = -1;
            goto exit;
        } 
#if 0
        else if (!remote_addr) {
            std::cerr << "ERROR - remote_addr not set - " << remote_addr << std::endl;
            ret = -1;
            goto exit;
        }
#endif
        // Get client address to send to server
        std::cout << "Client: Getting address to send to server" << std::endl;
        fi_getname((fid_t)ep,NULL, &addrlen);
        addr = malloc(addrlen);
        if(err = fi_getname((fid_t)ep,addr, &addrlen))
            goto exit;
        else if (addrlen <= 0) {
            std::cout << "Could not get client address" << std::endl;
            ret = -1;
            goto exit;
        }
        // Send name
        std::cout << "Client: Sending (" << addrlen << ") '" << addr << "' to " << remote_addr << std::endl;
        err = -FI_EAGAIN;
        while (err == -FI_EAGAIN) {
            err = fi_send(ep, addr, addrlen, NULL, remote_addr, NULL);
            if(err && (err != -FI_EAGAIN))
                goto exit;
        }
        // wait for server ack
        std::cout << "Client: Waiting for ack" << std::endl;
        if(err = wait_for_completion(rx_cq))
            goto exit;
        std::cout << "Client: Receiving" << std::endl;
        if (err = fi_recv(ep, remote_buf, max_msg_size, 0, 0, NULL))
            goto exit;
        // clear tx cq from first address sending
        std::cout << "Client: Waiting for Tx CQ completion" << std::endl;
        if (err = wait_for_completion(tx_cq))
            goto exit;
    } else {
        /* SERVER */
        // wait for client message containing address
        std::cout << "Server: Waiting for client to connect" << std::endl;
        if (err = wait_for_completion(rx_cq))
            goto exit;
        std::cout << "Server: Receiving client address" << std::endl;
        if (err = fi_recv(ep, remote_buf, max_msg_size, 0, 0, NULL)) goto exit;
        std::cout << "Server: Adding client to AV" << std::endl;
        if (1 != fi_av_insert(av, remote_buf, 1, &remote_addr, 0, NULL)) {
            std::cerr << "ERROR - fi_av_insert did not return 1" << std::endl;
            ret = -1;
            goto exit;
        }
        // send ack
        std::cout << "Server: Sending ack" << std::endl;
        err = -FI_EAGAIN;
        while (err == -FI_EAGAIN) {
            err = fi_send(ep, local_buf, 1, NULL, remote_addr, NULL);
            if (err && (err != -FI_EAGAIN))
                goto exit;
        }
        std::cout << "Server: Waiting for Tx CQ completion" << std::endl;
        wait_for_completion(tx_cq);
    }

    // Server and Client are ready, ping pong

    // Basic message sending protocol:
    //   Sender -
    //     send message with fi_send
    //     wait for Tx CQ to show it finished sending
    //   Receiver -
    //     wait for Rx CQ to show finished receiving
    //     get message with fi_recv

    if (dst_addr) {
        /* CLIENT */
        std::cout << "Client: Sending ping pong message" << std::endl;
        if(err = fi_send(ep, local_buf, max_msg_size, NULL, remote_addr, NULL)) goto exit;
        std::cout << "Client: Waiting for Tx completion" << std::endl;
        if (err = wait_for_completion(tx_cq)) goto exit;
        std::cout << "Client: Waiting for Rx completion" << std::endl;
        if (err = wait_for_completion(rx_cq)) goto exit;
        std::cout << "Client: Receiving message" << std::endl;
        if (err = fi_recv(ep, remote_buf, max_msg_size, 0, 0, NULL)) goto exit;
    } else {
        /* SERVER */
        std::cout << "Server: Waiting for Rx completion" << std::endl;
        if (err = wait_for_completion(rx_cq)) goto exit;
        std::cout << "Server: Receiving message" << std::endl;
        if (err = fi_recv(ep, remote_buf, max_msg_size, 0, 0, NULL)) goto exit;
        std::cout << "Server: Sending message" << std::endl;
        if (err = fi_send(ep, local_buf, max_msg_size, NULL, remote_addr, NULL)) goto exit;
        std::cout << "Server: Waiting for Tx completion" << std::endl;
        if (err = wait_for_completion(tx_cq)) goto exit;
    }


exit:

    if (err) {
        std::cerr << "ERROR (" << err << "): " << fi_strerror(-err) << std::endl;
        ret = err;
    }

    int rel_err = release_all();
    if (!ret && rel_err)
        ret = rel_err;
    return ret;
}
