/* MQTT 3.1.1 (QoS 0) publisher running in its own thread. */
#ifndef NEST_PICKER_MQTT_H
#define NEST_PICKER_MQTT_H

enum { CH_COLOR, CH_SOUND, NCH };

void mqtt_init(const char *prefix);  /* topics <prefix>/color, /sound, /status */
void mqtt_start(void);
void mqtt_stop(void);                /* after `running` is cleared */
void mq_set(int ch, const char *payload);  /* latest payload wins; published retained */
int mqtt_connected(void);
const char *mqtt_topic(int ch);

#endif
