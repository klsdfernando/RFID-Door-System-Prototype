from flask import Flask, render_template, request, redirect
import requests

app = Flask(__name__)

# The IP address of your ESP32 on your local network
ESP32_IP = "http://192.168.1.45" 

@app.route('/')
def index():
    try:
        # Fetch data directly from the ESP32's SD card database
        logs = requests.get(f"{ESP32_IP}/logs").json()
        members = requests.get(f"{ESP32_IP}/members").json()
    except Exception as e:
        print(f"Failed to connect to ESP32: {e}")
        logs = []
        members = []
        
    return render_template('index.html', logs=logs, members=members)

@app.route('/add_member', methods=['POST'])
def add_member():
    name = request.form.get('name')
    rfid = request.form.get('rfid')
    
    # Send the new member data to the ESP32 to save on the SD card
    if name and rfid:
        requests.post(f"{ESP32_IP}/add_member", data={'name': name, 'rfid': rfid})
        
    return redirect('/')

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000, debug=True)