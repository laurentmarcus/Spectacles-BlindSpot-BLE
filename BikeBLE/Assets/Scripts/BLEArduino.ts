// BleBikeBlindspot.ts
// Add this script to an empty SceneObject.
// In the Inspector, wire:
// - bluetoothModule: BluetoothCentralModule (drag from Resources)
// - statusText: optional Text component for debug
// - rightHud: Image (red icon anchored to right side, initially disabled)

@component
export class BleBikeBlindspot extends BaseScriptComponent {
  @input bluetoothModule: Bluetooth.BluetoothCentralModule;
  @input statusText: Text;
  @input rightHud: Image;

  private bluetoothGatt: Bluetooth.BluetoothGatt;
  private deviceName = "BikeBlindspot";
  private serviceUUID = "0000ffe5-0000-1000-8000-00805f9b34fb";
  private characteristicUUID = "0000ffe6-0000-1000-8000-00805f9b34fb";

  onAwake() {
    if (this.rightHud) this.rightHud.enabled = false;
    if (!global.deviceInfoSystem.isEditor()) {
      this.scanAndConnect();
    } else {
      this.log("Run on Spectacles to use Bluetooth.");
    }
  }

  private log(msg: string) {
    if (this.statusText) this.statusText.text = msg;
    print(`[BLE] ${msg}`);
  }

  private scanAndConnect() {
    this.log("Scanning for BikeBlindspot…");

    let filter = new Bluetooth.ScanFilter();
    filter.deviceName = this.deviceName; // case-sensitive

    let settings = new Bluetooth.ScanSettings();
    settings.uniqueDevices = true;
    settings.timeoutSeconds = 20;
    settings.scanMode = Bluetooth.ScanMode.LowPower;

    this.bluetoothModule.startScan([filter], settings, (result) => {
      // return true to stop scanning when we find our device
      return result.deviceName === this.deviceName;
    }).then(async (scanResult) => {
      this.log(`Found ${scanResult.deviceName}, connecting…`);
      const gatt = await this.bluetoothModule.connectGatt(scanResult.deviceAddress);
      this.bluetoothGatt = gatt as Bluetooth.BluetoothGatt;

      // Optional: enumerate services/characteristics for debugging
      this.bluetoothGatt.getServices().forEach(s => {
        this.log(`Service: ${s.uuid}`);
        s.getCharacteristics().forEach(c => this.log(`  Char: ${c.uuid}`));
      });

      // Get our char and subscribe
      const service = this.bluetoothGatt.getService(this.serviceUUID);
      const characteristic = service.getCharacteristic(this.characteristicUUID);

      await characteristic.registerNotifications((val: Uint8Array) => this.onNotify(val));
      this.log("Subscribed to distance notifications.");
    }).catch((err) => {
      this.log(`Scan/connect error: ${err}`);
    });
  }

  private onNotify(val: Uint8Array) {
    if (!val || val.length < 3) return;
    const cm = val[0] | (val[1] << 8);
    const alert = val[2] === 1;

    // Simple HUD logic: show red badge on the RIGHT when alert
    if (this.rightHud) this.rightHud.enabled = alert;

    // Optional debug readout
    this.log(`Distance: ${cm}cm`);
    // this.log(`Distance: ${cm}cm | Alert: ${alert ? "YES" : "no"}`);
  }
}
