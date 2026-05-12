import paho.mqtt.client as mqtt
import time

BROKER = "7079618d65c446c28ed8db5aeeba2a82.s1.eu.hivemq.cloud"
PORT = 8883
USERNAME = "Ripple"
PASSWORD = "Ripple1234"
TOPIC = "esp32/ota/command"
COMMAND = '{"version":"1.0.9","checksum":"7e05297d804930f42e58aad00738f86053ee40913b0c860652c6b6dda20a2442","size":1155184}'

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
