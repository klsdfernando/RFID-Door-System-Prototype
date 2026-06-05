from flask import Flask, render_template, request, redirect, jsonify
import requests

app = Flask(__name__)

# The ESP32 IP address
ESP32_IP = "http://192.168.1.45" 

@app.route('/')
def index():
    try:
        logs = requests.get(f"{ESP32_IP}/logs", timeout=5).json()
        members = requests.get(f"{ESP32_IP}/members", timeout=5).json()
    except Exception as e:
        print(f"Failed to connect to ESP32: {e}")
        logs = []
        members = []
        
    return render_template('index.html', logs=logs, members=members)

# --- Background API for JavaScript Auto-Refresh ---
@app.route('/api/data')
def get_data():
    try:
        logs = requests.get(f"{ESP32_IP}/logs", timeout=5).json()
        members = requests.get(f"{ESP32_IP}/members", timeout=5).json()
        return jsonify({"logs": logs, "members": members})
    except Exception as e:
        return jsonify({"logs": [], "members": []}), 500

@app.route('/add_member', methods=['POST'])
def add_member():
    member_id = request.form.get('member_id')
    name = request.form.get('name')
    rfid = request.form.get('rfid')
    
    if member_id and name and rfid:
        try:
            requests.post(f"{ESP32_IP}/add_member", data={'member_id': member_id, 'name': name, 'rfid': rfid}, timeout=5)
        except Exception as e:
            print(f"Error adding member: {e}")
            
    return redirect('/')

@app.route('/delete_member', methods=['POST'])
def delete_member():
    member_id = request.form.get('member_id')
    
    if member_id:
        try:
            requests.post(f"{ESP32_IP}/delete_member", data={'member_id': member_id}, timeout=5)
        except Exception as e:
            print(f"Error deleting member: {e}")
            
    return redirect('/')

@app.route('/reset_db', methods=['POST'])
def reset_db():
    try:
        requests.get(f"{ESP32_IP}/reset_db", timeout=10)
    except Exception as e:
        print(f"Error resetting DB: {e}")
    return redirect('/')

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000, debug=True)