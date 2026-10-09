/****************************************************************************
 * pnut-os/src/lib/include/pnut/topic.h
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Mateusz Pianka
 *
 ****************************************************************************/

#ifndef __PNUT_OS_LIB_PNUT_TOPIC_H
#define __PNUT_OS_LIB_PNUT_TOPIC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdint.h>

#include <pnut/compiler.h>
#include <pnut/loop.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* A topic (RFC 0005): fixed-size messages that any native program may
 * publish and read, for state and events meant for everyone.  On NuttX it
 * is a uORB topic; on the computer, local datagram sockets under the
 * loop's run directory stand in for it.
 *
 * A reader is called on its loop with every message, oldest first, and
 * with the latest one published before it subscribed, if any.  uORB keeps
 * depth messages for a reader who has not read them, and a newer one
 * takes the oldest's place, so a topic of events carries a sequence number
 * of its own, and a reader who sees a gap reads the state again.  On the
 * computer, a reader's socket queue is the bound instead, and a message
 * that finds it full is the one lost: the gap shows the same way.
 *
 * The generator writes a topic's description and its typed functions from
 * its .proto file (option (pnut.topic)).
 */

struct pnut_topic_s
{
  FAR const char *name;           /* Lowercase letters, digits, underscores */
  uint16_t size;                  /* A message's size, in bytes */
  uint8_t depth;                  /* Messages kept for a reader */
  FAR const void *native;         /* uORB's description, on NuttX */
};

struct pnut_publisher_s;
struct pnut_subscriber_s;

/* Called on the loop with a message, valid while it runs */

typedef CODE void (*pnut_topic_handler_t)(FAR struct pnut_subscriber_s *sub,
                                          FAR const void *msg,
                                          FAR void *arg);

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: pnut_topic_advertise
 *
 * Description:
 *   Become a publisher of a topic.  It allocates: call it as the program
 *   starts, from a module's start (RFC 0023).
 *
 * Returned Value:
 *   Zero (OK) on success; -ENOSYS on NuttX without uORB; another negated
 *   errno value on failure.
 *
 ****************************************************************************/

int pnut_topic_advertise(FAR struct pnut_loop_s *loop,
                         FAR const struct pnut_topic_s *topic,
                         FAR struct pnut_publisher_s **pubp);

/****************************************************************************
 * Name: pnut_topic_unadvertise
 ****************************************************************************/

void pnut_topic_unadvertise(FAR struct pnut_publisher_s *pub);

/****************************************************************************
 * Name: pnut_topic_publish
 *
 * Description:
 *   Publish a message, topic->size bytes, copied.  A reader who has not
 *   read the topic's depth of messages misses one (see above).
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int pnut_topic_publish(FAR struct pnut_publisher_s *pub,
                       FAR const void *msg);

/****************************************************************************
 * Name: pnut_topic_subscribe
 *
 * Description:
 *   Read a topic on the loop: handler is called with each message.  It
 *   may unsubscribe from its own handler.  It allocates: call it as the
 *   program starts, from a module's start (RFC 0023).
 *
 * Returned Value:
 *   Zero (OK) on success; -ENOSYS on NuttX without uORB; -EBUSY when the
 *   loop watches as many descriptors as it can; another negated errno
 *   value on failure.
 *
 ****************************************************************************/

int pnut_topic_subscribe(FAR struct pnut_loop_s *loop,
                         FAR const struct pnut_topic_s *topic,
                         pnut_topic_handler_t handler, FAR void *arg,
                         FAR struct pnut_subscriber_s **subp);

/****************************************************************************
 * Name: pnut_topic_unsubscribe
 ****************************************************************************/

void pnut_topic_unsubscribe(FAR struct pnut_subscriber_s *sub);

#endif /* __PNUT_OS_LIB_PNUT_TOPIC_H */
