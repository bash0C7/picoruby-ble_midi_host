import CoreBluetooth
import CBLEMIDIHost

final class BLEMIDICentral: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
  static let shared = BLEMIDICentral()
  static let serviceID = CBUUID(string: "03B80E5A-EDE8-4B33-A751-6CE34EC4C700")
  static let characteristicID = CBUUID(string: "7772E5DB-3868-4112-A1A9-F2669D106BF3")
  static let knownKey = "picoruby.ble_midi_host.known"

  private struct Known {
    let key: UInt64
    let identifier: UUID
    var name: String
  }

  private var manager: CBCentralManager?
  private var seen: [UInt64: CBPeripheral] = [:]
  private var known: [Known] = []
  private var slots: [Int32: CBPeripheral] = [:]
  private var characteristics: [Int32: CBCharacteristic] = [:]

  static func address(_ identifier: UUID) -> [UInt8] {
    withUnsafeBytes(of: identifier.uuid) { Array($0.prefix(6)) }
  }

  static func key(_ address: [UInt8]) -> UInt64 {
    address.reduce(0) { ($0 << 8) | UInt64($1) }
  }

  static func key(_ address: UnsafePointer<UInt8>) -> UInt64 {
    key(Array(UnsafeBufferPointer(start: address, count: 6)))
  }

  func load() {
    let entries = UserDefaults.standard.array(forKey: Self.knownKey) as? [[String: String]] ?? []
    known = entries.compactMap { entry in
      guard let text = entry["id"], let identifier = UUID(uuidString: text) else { return nil }
      return Known(key: Self.key(Self.address(identifier)), identifier: identifier, name: entry["name"] ?? "")
    }
    for entry in known {
      BLE_MIDI_HOST_darwin_known(Self.address(entry.identifier), entry.name)
    }
  }

  func power(_ on: Bool) {
    guard on else {
      manager?.stopScan()
      return
    }
    guard let manager else {
      manager = CBCentralManager(delegate: self, queue: BLE_MIDI_HOST_darwin_queue())
      return
    }
    if manager.state == .poweredOn { BLE_MIDI_HOST_darwin_powered(true) }
  }

  func scan(_ on: Bool) {
    guard let manager, manager.state == .poweredOn else { return }
    if on {
      manager.scanForPeripherals(withServices: [Self.serviceID],
                                 options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
    } else {
      manager.stopScan()
    }
  }

  func connect(slot: Int32, key: UInt64) -> Bool {
    guard let manager, manager.state == .poweredOn else { return false }
    var peripheral = seen[key]
    if peripheral == nil, let entry = known.first(where: { $0.key == key }) {
      peripheral = manager.retrievePeripherals(withIdentifiers: [entry.identifier]).first
    }
    guard let peripheral else { return false }
    peripheral.delegate = self
    slots[slot] = peripheral
    manager.connect(peripheral, options: nil)
    return true
  }

  func cancel(slot: Int32) {
    characteristics[slot] = nil
    guard let peripheral = slots.removeValue(forKey: slot) else { return }
    manager?.cancelPeripheralConnection(peripheral)
  }

  func write(slot: Int32, data: Data) -> Bool {
    guard let peripheral = slots[slot], let characteristic = characteristics[slot] else { return true }
    guard peripheral.canSendWriteWithoutResponse else { return false }
    peripheral.writeValue(data, for: characteristic, type: .withoutResponse)
    return true
  }

  func remember(key: UInt64, name: String) {
    if let index = known.firstIndex(where: { $0.key == key }) {
      known[index].name = name
    } else if let identifier = seen[key]?.identifier {
      known.append(Known(key: key, identifier: identifier, name: name))
    }
    save()
  }

  func forget(key: UInt64) {
    known.removeAll { $0.key == key }
    save()
  }

  private func save() {
    let entries = known.map { ["id": $0.identifier.uuidString, "name": $0.name] }
    UserDefaults.standard.set(entries, forKey: Self.knownKey)
  }

  private func slot(of peripheral: CBPeripheral) -> Int32? {
    slots.first { $0.value.identifier == peripheral.identifier }?.key
  }

  private func fail(_ peripheral: CBPeripheral, code: Int32) {
    guard let slot = slot(of: peripheral) else { return }
    cancel(slot: slot)
    BLE_MIDI_HOST_darwin_failed(slot, code)
  }

  private static func code(_ error: Error?) -> Int32 {
    guard let error else { return -3 }
    return Int32(truncatingIfNeeded: (error as NSError).code)
  }

  func centralManagerDidUpdateState(_ central: CBCentralManager) {
    let on = central.state == .poweredOn
    if !on {
      slots.removeAll()
      characteristics.removeAll()
    }
    BLE_MIDI_HOST_darwin_powered(on)
  }

  func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                      advertisementData: [String: Any], rssi RSSI: NSNumber) {
    let address = Self.address(peripheral.identifier)
    seen[Self.key(address)] = peripheral
    let name = advertisementData[CBAdvertisementDataLocalNameKey] as? String ?? peripheral.name ?? ""
    BLE_MIDI_HOST_darwin_discovered(address, name, RSSI.int32Value)
  }

  func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
    guard let slot = slot(of: peripheral) else {
      central.cancelPeripheralConnection(peripheral)
      return
    }
    BLE_MIDI_HOST_darwin_linked(slot)
    peripheral.discoverServices([Self.serviceID])
  }

  func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
    released(peripheral, code: Self.code(error))
  }

  func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
    released(peripheral, code: error == nil ? 0 : Self.code(error))
  }

  private func released(_ peripheral: CBPeripheral, code: Int32) {
    guard let slot = slot(of: peripheral) else { return }
    slots[slot] = nil
    characteristics[slot] = nil
    BLE_MIDI_HOST_darwin_failed(slot, code)
  }

  func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
    guard error == nil, let service = peripheral.services?.first(where: { $0.uuid == Self.serviceID }) else {
      fail(peripheral, code: Self.code(error))
      return
    }
    peripheral.discoverCharacteristics([Self.characteristicID], for: service)
  }

  func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
    guard let slot = slot(of: peripheral) else { return }
    guard error == nil,
          let characteristic = service.characteristics?.first(where: { $0.uuid == Self.characteristicID }) else {
      fail(peripheral, code: Self.code(error))
      return
    }
    characteristics[slot] = characteristic
    BLE_MIDI_HOST_darwin_subscribing(slot)
    peripheral.setNotifyValue(true, for: characteristic)
  }

  func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic,
                  error: Error?) {
    guard let slot = slot(of: peripheral) else { return }
    guard error == nil, characteristic.isNotifying else {
      fail(peripheral, code: Self.code(error))
      return
    }
    BLE_MIDI_HOST_darwin_subscribed(slot, UInt16(clamping: peripheral.maximumWriteValueLength(for: .withoutResponse)))
  }

  func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
    guard error == nil, let slot = slot(of: peripheral), let value = characteristic.value, !value.isEmpty else { return }
    value.withUnsafeBytes { bytes in
      BLE_MIDI_HOST_darwin_notified(slot, bytes.bindMemory(to: UInt8.self).baseAddress, Int32(value.count))
    }
  }

  func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) {
    guard let slot = slot(of: peripheral) else { return }
    BLE_MIDI_HOST_darwin_ready(slot)
  }
}

@_cdecl("bmh_swift_load")
public func bmhSwiftLoad() {
  BLEMIDICentral.shared.load()
}

@_cdecl("bmh_swift_power")
public func bmhSwiftPower(_ on: Bool) {
  BLEMIDICentral.shared.power(on)
}

@_cdecl("bmh_swift_scan")
public func bmhSwiftScan(_ on: Bool) {
  BLEMIDICentral.shared.scan(on)
}

@_cdecl("bmh_swift_connect")
public func bmhSwiftConnect(_ slot: Int32, _ address: UnsafePointer<UInt8>) -> Bool {
  BLEMIDICentral.shared.connect(slot: slot, key: BLEMIDICentral.key(address))
}

@_cdecl("bmh_swift_cancel")
public func bmhSwiftCancel(_ slot: Int32) {
  BLEMIDICentral.shared.cancel(slot: slot)
}

@_cdecl("bmh_swift_write")
public func bmhSwiftWrite(_ slot: Int32, _ data: UnsafePointer<UInt8>, _ length: Int32) -> Bool {
  BLEMIDICentral.shared.write(slot: slot, data: Data(bytes: data, count: Int(length)))
}

@_cdecl("bmh_swift_remember")
public func bmhSwiftRemember(_ address: UnsafePointer<UInt8>, _ name: UnsafePointer<CChar>) {
  BLEMIDICentral.shared.remember(key: BLEMIDICentral.key(address), name: String(cString: name))
}

@_cdecl("bmh_swift_forget")
public func bmhSwiftForget(_ address: UnsafePointer<UInt8>) {
  BLEMIDICentral.shared.forget(key: BLEMIDICentral.key(address))
}
