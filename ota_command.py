import paho.mqtt.client as mqtt
import time

BROKER = "7079618d65c446c28ed8db5aeeba2a82.s1.eu.hivemq.cloud"
PORT = 8883
USERNAME = "Ripple"
PASSWORD = "Ripple1234"
TOPIC = "esp32/ota/command"
COMMAND = '{"version":"1.0.8","checksum":"044bf82bdf5e0e36e1a68f151549b142a922c9e8a781017cdde0eab8b7b63170","size":1155104}'

client = mqtt.Client()
client.username_pw_set(USERNAME, PASSWORD)
client.tls_set()
client.connect(BROKER, PORT)

client.loop_start()
time.sleep(1)

info = client.publish(TOPIC, COMMAND, qos=1)
info.wait_for_publish()
print("OTA Command Sent – check serial monitor!")

client.loop_stop()
client.disconnect()
