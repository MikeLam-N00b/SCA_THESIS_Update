import requests

# Server configuration
SERVER_URL = "http://10.0.7.178:8000"

# Static Vehicle ID
VEHICLE_ID = "VIN123456"


def check_pairing_status():
    """Check if the vehicle is paired on the server"""
    try:
        # Send GET request to check pairing endpoint
        url = f"{SERVER_URL}/check-pairing/{VEHICLE_ID}"

        print("\n" + "=" * 50)
        print("Smart Car Access - Pairing Status Checker")
        print("=" * 50)
        print(f"Vehicle ID: {VEHICLE_ID}")
        print(f"Server:     {SERVER_URL}")
        print("=" * 50 + "\n")

        print(f"Checking pairing status...")
        print(f"URL: {url}\n")

        response = requests.get(url)

        if response.status_code == 200:
            data = response.json()

            print("=" * 50)
            if data["paired"]:
                print("✓ VEHICLE IS PAIRED")
                print("=" * 50)
                print(f"Vehicle ID:  {data['vehicle_id']}")
                print(f"Pairing ID:  {data['pairing_id']}")
                print(f"Paired At:   {data['paired_at']}")
                print("=" * 50)
                return True
            else:
                print("✗ VEHICLE NOT PAIRED")
                print("=" * 50)
                print(f"Vehicle ID:  {data['vehicle_id']}")
                print(f"Message:     {data['message']}")
                print("=" * 50)
                return False
        else:
            print(f"❌ Error: HTTP {response.status_code}")
            print(f"Response: {response.text}")
            return False

    except requests.exceptions.ConnectionError:
        print("❌ Error: Cannot connect to server")
        print(f"Make sure the server is running at {SERVER_URL}")
        return False
    except Exception as e:
        print(f"❌ Error: {str(e)}")
        return False


if __name__ == "__main__":
    check_pairing_status()