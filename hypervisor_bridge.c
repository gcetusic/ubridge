/*
 *   This file is part of ubridge, a program to bridge network interfaces
 *   to UDP tunnels.
 *
 *   Copyright (C) 2015 GNS3 Technologies Inc.
 *
 *   ubridge is free software: you can redistribute it and/or modify it
 *   under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   ubridge is distributed in the hope that it will be useful, but
 *   WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <assert.h>

#include "ubridge.h"
#include "nio.h"
#include "nio_udp.h"
#include "nio_tap.h"
#include "nio_ethernet.h"
#ifdef LINUX_RAW
#include "nio_linux_raw.h"
#endif
#include "hypervisor.h"
#include "hypervisor_bridge.h"
#include "pcap_capture.h"

static bridge_t *find_bridge(char *bridge_name)
{
   bridge_t *bridge;
   bridge_t *next;

   bridge = bridge_list;
   while (bridge != NULL) {
     if (!strcmp(bridge->name, bridge_name))
         return bridge;
     next = bridge->next;
     bridge = next;
   }
   return (NULL);
}

static int cmd_create_bridge(hypervisor_conn_t *conn, int argc, char *argv[])
{
   bridge_t *new_bridge;
   bridge_t **head;

   if (find_bridge(argv[0]) != NULL) {
      hypervisor_send_reply(conn, HSC_ERR_CREATE, 1, "bridge '%s' already exist", argv[0]);
      return (-1);
   }

   head = &bridge_list;
   if ((new_bridge = malloc(sizeof(*new_bridge))) == NULL)
      goto memory_error;
   if ((new_bridge->name = strdup(argv[0])) == NULL)
      goto memory_error;
   new_bridge->running = FALSE;
   new_bridge->source_nio = NULL;
   new_bridge->destination_nio = NULL;
   new_bridge->capture = NULL;
   new_bridge->next = *head;
   *head = new_bridge;
   hypervisor_send_reply(conn, HSC_INFO_OK, 1, "bridge '%s' created", argv[0]);
   return (0);

   memory_error:
   hypervisor_send_reply(conn, HSC_ERR_CREATE, 1, "could not create bridge '%s': insufficient memory", argv[0]);
   return (-1);
}

static int cmd_delete_bridge(hypervisor_conn_t *conn, int argc, char *argv[])
{
    bridge_t **head;
    bridge_t *bridge;
    bridge_t *prev = NULL;

    head = &bridge_list;
    for (bridge = *head; bridge != NULL; prev = bridge, bridge = bridge->next) {
       if (!strcmp(bridge->name, argv[0])) {
          if (prev == NULL)
             *head = bridge->next;
          else
             prev->next = bridge->next;

          if (bridge->name)
             free(bridge->name);
          if (bridge->running) {
             pthread_cancel(bridge->source_tid);
             pthread_join(bridge->source_tid, NULL);
             pthread_cancel(bridge->destination_tid);
             pthread_join(bridge->destination_tid, NULL);
          }
          free_nio(bridge->source_nio);
          free_nio(bridge->destination_nio);
          free_pcap_capture(bridge->capture);
          free(bridge);
          hypervisor_send_reply(conn, HSC_INFO_OK, 1, "bridge '%s' deleted", argv[0]);
          return (0);
      }
   }
   hypervisor_send_reply(conn, HSC_ERR_NOT_FOUND, 1, "bridge '%s' doesn't exist", argv[0]);
   return (-1);
}

static int cmd_start_bridge(hypervisor_conn_t *conn, int argc, char *argv[])
{
   bridge_t *bridge;
   int s;

   bridge = find_bridge(argv[0]);
   if (bridge == NULL) {
      hypervisor_send_reply(conn, HSC_ERR_NOT_FOUND, 1, "bridge '%s' doesn't exist", argv[0]);
      return (-1);
   }

   if (bridge->running) {
      hypervisor_send_reply(conn, HSC_ERR_START, 1, "bridge '%s' is already running", argv[0]);
      return (-1);
   }

   if (!(bridge->source_nio && bridge->destination_nio)) {
      hypervisor_send_reply(conn, HSC_ERR_START, 1, "bridge '%s' must have 2 NIOs to be started", argv[0]);
      return (-1);
   }

   s = pthread_create(&(bridge->source_tid), NULL, &source_nio_listener, bridge);
   if (s != 0) {
      handle_error_en(s, "pthread_create");
      hypervisor_send_reply(conn, HSC_ERR_START, 1, "cannot create source NIO thread for bridge '%s'", argv[0]);
      return (-1);
   }

   s = pthread_create(&(bridge->destination_tid), NULL, &destination_nio_listener, bridge);
   if (s != 0) {
      handle_error_en(s, "pthread_create");
      hypervisor_send_reply(conn, HSC_ERR_START, 1, "cannot create destination NIO thread for bridge '%s'", argv[0]);
      return (-1);
   }

   bridge->running = TRUE;
   hypervisor_send_reply(conn, HSC_INFO_OK, 1, "bridge '%s' started", argv[0]);
   return (0);
}

static int cmd_stop_bridge(hypervisor_conn_t *conn, int argc, char *argv[])
{
   bridge_t *bridge;

   bridge = find_bridge(argv[0]);
   if (bridge == NULL) {
      hypervisor_send_reply(conn, HSC_ERR_NOT_FOUND, 1, "bridge '%s' doesn't exist", argv[0]);
      return (-1);
   }

   if (!bridge->running) {
      hypervisor_send_reply(conn, HSC_ERR_STOP, 1, "bridge '%s' is not running", argv[0]);
      return (-1);
   }

   pthread_cancel(bridge->source_tid);
   pthread_join(bridge->source_tid, NULL);
   pthread_cancel(bridge->destination_tid);
   pthread_join(bridge->destination_tid, NULL);
   bridge->running = FALSE;
   hypervisor_send_reply(conn, HSC_INFO_OK, 1, "bridge '%s' stopped", argv[0]);
   return (0);
}

static int cmd_rename_bridge(hypervisor_conn_t *conn, int argc, char *argv[])
{
   bridge_t *bridge;
   char *newname;

   bridge = find_bridge(argv[0]);
   if (bridge == NULL) {
      hypervisor_send_reply(conn, HSC_ERR_NOT_FOUND, 1, "bridge '%s' doesn't exist", argv[0]);
      return (-1);
   }

   if (find_bridge(argv[1]) != NULL) {
      hypervisor_send_reply(conn, HSC_ERR_RENAME, 1, "bridge '%s' already exist", argv[0]);
      return (-1);
   }

   if(!(newname = strdup(argv[1]))) {
      hypervisor_send_reply(conn, HSC_ERR_RENAME, 1, "unable to rename bridge '%s', out of memory", argv[0]);
      return(-1);
   }

   if (bridge->name)
       free(bridge->name);
   bridge->name = newname;
   hypervisor_send_reply(conn, HSC_INFO_OK, 1, "bridge '%s' renamed to '%s'", argv[0], argv[1]);
   return (0);
}

static int cmd_list_bridges(hypervisor_conn_t *conn, int argc, char *argv[])
{
   bridge_t *bridge;
   bridge_t *next;
   int nios;

   bridge = bridge_list;
   while (bridge != NULL) {
     nios = 0;
     if (bridge->source_nio)
        nios += 1;
     if (bridge->destination_nio)
        nios += 1;
     hypervisor_send_reply(conn, HSC_INFO_MSG, 0, "%s (NIOs = %d)", bridge->name, nios);
     next = bridge->next;
     bridge = next;
   }
   hypervisor_send_reply(conn, HSC_INFO_OK, 1, "OK");
   return (0);
}

static int add_nio_to_bridge(hypervisor_conn_t *conn, bridge_t *bridge, nio_t *nio)
{
   if (bridge->source_nio && bridge->destination_nio) {
      hypervisor_send_reply(conn, HSC_ERR_CREATE, 1, "bridge '%s' has already 2 allocated NIOs", bridge->name);
      return (-1);
   }

   if (bridge->source_nio == NULL)
      bridge->source_nio = nio;
   else if (bridge->destination_nio == NULL)
      bridge->destination_nio = nio;
   else {
      /* should not happen */
      hypervisor_send_reply(conn, HSC_ERR_CREATE, 1, "bridge '%s': no NIO slot available", bridge->name);
      return (-1);
   }
   return (0);
}

static int cmd_add_nio_udp(hypervisor_conn_t *conn, int argc, char *argv[])
{
   nio_t *nio;
   bridge_t *bridge;

   bridge = find_bridge(argv[0]);
   if (bridge == NULL) {
      hypervisor_send_reply(conn, HSC_ERR_NOT_FOUND, 1, "bridge '%s' doesn't exist", argv[0]);
      return (-1);
   }

   nio = create_nio_udp(atoi(argv[1]), argv[2], atoi(argv[3]));
   if (!nio) {
      hypervisor_send_reply(conn, HSC_ERR_CREATE, 1, "unable to create NIO UDP for bridge '%s'", argv[0]);
      return (-1);
   }

   if (add_nio_to_bridge(conn, bridge, nio) == -1) {
     free_nio(nio);
     return (-1);
   }

   hypervisor_send_reply(conn, HSC_INFO_OK,1, "NIO UDP added to bridge '%s'", argv[0]);
   return (0);
}

static int cmd_add_nio_tap(hypervisor_conn_t *conn, int argc, char *argv[])
{
   nio_t *nio;
   bridge_t *bridge;

   bridge = find_bridge(argv[0]);
   if (bridge == NULL) {
      hypervisor_send_reply(conn, HSC_ERR_NOT_FOUND, 1, "bridge '%s' doesn't exist", argv[0]);
      return (-1);
   }

   nio = create_nio_tap(argv[1]);
   if (!nio) {
      hypervisor_send_reply(conn, HSC_ERR_CREATE, 1, "unable to create NIO TAP for bridge '%s'", argv[0]);
      return (-1);
   }

   if (add_nio_to_bridge(conn, bridge, nio) == -1) {
     free_nio(nio);
     return (-1);
   }

   hypervisor_send_reply(conn, HSC_INFO_OK, 1, "NIO TAP added to bridge '%s'", argv[0]);
   return (0);
}

static int cmd_add_nio_ethernet(hypervisor_conn_t *conn, int argc, char *argv[])
{
   nio_t *nio;
   bridge_t *bridge;

   bridge = find_bridge(argv[0]);
   if (bridge == NULL) {
      hypervisor_send_reply(conn, HSC_ERR_NOT_FOUND, 1, "bridge '%s' doesn't exist", argv[0]);
      return (-1);
   }

   nio = create_nio_ethernet(argv[1]);
   if (!nio) {
      hypervisor_send_reply(conn, HSC_ERR_CREATE, 1, "unable to create NIO Ethernet for bridge '%s'", argv[0]);
      return (-1);
   }

   if (add_nio_to_bridge(conn, bridge, nio) == -1) {
     free_nio(nio);
     return (-1);
   }

   hypervisor_send_reply(conn, HSC_INFO_OK,1, "NIO Ethernet added to bridge '%s'", argv[0]);
   return (0);
}

#ifdef LINUX_RAW
static int cmd_add_nio_linux_raw(hypervisor_conn_t *conn, int argc, char *argv[])
{
   nio_t *nio;
   bridge_t *bridge;

   bridge = find_bridge(argv[0]);
   if (bridge == NULL) {
      hypervisor_send_reply(conn, HSC_ERR_NOT_FOUND, 1, "bridge '%s' doesn't exist", argv[0]);
      return (-1);
   }

   nio = create_nio_linux_raw(argv[1]);
   if (!nio) {
      hypervisor_send_reply(conn, HSC_ERR_CREATE, 1, "unable to create NIO Linux raw for bridge '%s'", argv[0]);
      return (-1);
   }

   if (add_nio_to_bridge(conn, bridge, nio) == -1) {
     free_nio(nio);
     return (-1);
   }

   hypervisor_send_reply(conn, HSC_INFO_OK, 1, "NIO Linux raw added to bridge '%s'", argv[0]);
   return (0);
}
#endif

static int cmd_start_capture_bridge(hypervisor_conn_t *conn, int argc, char *argv[])
{
   char *pcap_linktype = "EN10MB";
   bridge_t *bridge;

   bridge = find_bridge(argv[0]);
   if (bridge == NULL) {
      hypervisor_send_reply(conn, HSC_ERR_NOT_FOUND, 1, "bridge '%s' doesn't exist", argv[0]);
      return (-1);
   }

   if (bridge->capture != NULL) {
      hypervisor_send_reply(conn, HSC_ERR_START, 1, "packet capture is already active on bridge '%s'", argv[0]);
      return (-1);
   }

   if (argc == 3)
     pcap_linktype = argv[2];

   if (!(bridge->capture = create_pcap_capture(argv[1], pcap_linktype))) {
      hypervisor_send_reply(conn, HSC_ERR_START, 1, "packet capture could not be started on bridge '%s'", argv[0]);
      return (-1);
   }

   hypervisor_send_reply(conn, HSC_INFO_OK, 1, "packet capture started on bridge '%s'", argv[0]);
   return (0);
}

static int cmd_stop_capture_bridge(hypervisor_conn_t *conn, int argc, char *argv[])
{
   bridge_t *bridge;

   bridge = find_bridge(argv[0]);
   if (bridge == NULL) {
      hypervisor_send_reply(conn, HSC_ERR_NOT_FOUND, 1, "bridge '%s' doesn't exist", argv[0]);
      return (-1);
   }

   if (bridge->capture == NULL) {
      hypervisor_send_reply(conn, HSC_ERR_START, 1, "no packet capture active on bridge '%s'", argv[0]);
      return (-1);
   }

   free_pcap_capture(bridge->capture);
   bridge->capture = NULL;
   hypervisor_send_reply(conn, HSC_INFO_OK, 1, "packet capture stopped on bridge '%s'", argv[0]);
   return (0);
}


/* Bridge commands */
static hypervisor_cmd_t bridge_cmd_array[] = {
   { "create", 1, 1, cmd_create_bridge, NULL },
   { "delete", 1, 1, cmd_delete_bridge, NULL },
   { "start", 1, 1, cmd_start_bridge, NULL },
   { "stop", 1, 1, cmd_stop_bridge, NULL },
   { "rename", 2, 2, cmd_rename_bridge, NULL },
   { "add_nio_udp", 4, 4, cmd_add_nio_udp, NULL },
   { "add_nio_tap", 2, 2, cmd_add_nio_tap, NULL },
   { "add_nio_ethernet", 2, 2, cmd_add_nio_ethernet, NULL },
#ifdef LINUX_RAW
   { "add_nio_linux_raw", 2, 2, cmd_add_nio_linux_raw, NULL },
#endif
   { "start_capture", 2, 3, cmd_start_capture_bridge, NULL },
   { "stop_capture", 1, 1, cmd_stop_capture_bridge, NULL },
   { "list", 0, 0, cmd_list_bridges, NULL },
   { NULL, -1, -1, NULL, NULL },
};

/* Hypervisor bridge initialization */
int hypervisor_bridge_init(void)
{
   hypervisor_module_t *module;

   module = hypervisor_register_module("bridge", NULL);
   assert(module != NULL);

   hypervisor_register_cmd_array(module, bridge_cmd_array);
   return(0);
}
