import paho.mqtt.client as mqtt
import sys, time, math

BROKER = "7079618d65c446c28ed8db5aeeba2a82.s1.eu.hivemq.cloud"
PORT = 8883
USERNAME = "Ripple"
PASSWORD = "Ripple1234"
TOPIC = "esp32/ota/chunk"
CHUNK_SIZE = 512
FLUSH_WAIT = 20   # seconds to keep connection alive after last chunk

def chunk_file(filename):
    with open(filename, "rb") as f:
        data = f.read()
    for i in range(0, len(data), CHUNK_SIZE):
        yield data[i:i+CHUNK_SIZE]

def main():
    if len(sys.argv) < 2:
        print("Usage: python ota_sender.py firmware.bin")
        sys.exit(1)

    filename = sys.argv[1]
    client = mqtt.Client()
    client.username_pw_set(USERNAME, PASSWORD)
    client.tls_set()
    client.connect(BROKER, PORT)

    client.loop_start()
    time.sleep(1)

    for idx, chunk in enumerate(chunk_file(filename)):
        info = client.publish(TOPIC, chunk, qos=1)
        info.wait_for_publish()
        print(f"Chunk {idx+1} sent ({len(chunk)} bytes)")
        time.sleep(0.8)          # slower = safer

    print("All chunks published. Waiting to ensure delivery...")
    time.sleep(FLUSH_WAIT)
    client.loop_stop()
    client.disconnect()
    print("Done. Device will verify and reboot.")

if __name__ == "__main__":
    main()
