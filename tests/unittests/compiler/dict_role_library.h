// The role-bound library dictionary of the dictionary role tests (decision 0027): `Table<K, V>` over the runtime
// dictionary ABI, in pieces a malformed-shape test can replace. Shared by the role tests that replace the collections
// prelude and mention a dictionary.
#ifndef JOYEER_TESTS_DICT_ROLE_LIBRARY_H
#define JOYEER_TESTS_DICT_ROLE_LIBRARY_H

#include <string>

namespace joyeer::fixtures {

// The runtime dictionary ABI the table is written over (decision 0027 option A).
inline constexpr auto tableExterns = R"(
@c_extern(symbol: "joyeer_dictionary_create_owned_abi")
private func createTable(output: Int, entries: Int, count: Int, keySize: Int, valueSize: Int,
        entrySize: Int, valueOffset: Int, keyKind: Int32, cloneKey: Int, destroyKey: Int,
        cloneValue: Int, destroyValue: Int): Void

@c_extern(symbol: "joyeer_dictionary_clone_abi")
private func cloneTable(output: Int, data: Int, count: Int): Void

@c_extern(symbol: "joyeer_dictionary_set_owned_abi")
private func setOwned(dictionary: Int, key: Int, value: Int): Void

@c_extern(symbol: "joyeer_dictionary_find_abi")
private func findValue(data: Int, count: Int, key: Int, keySize: Int, keyKind: Int32): Int

@c_extern(symbol: "joyeer_dictionary_at_abi")
private func valueAt(data: Int, count: Int, key: Int, keySize: Int, keyKind: Int32): Int

@c_extern(symbol: "joyeer_dictionary_destroy_abi")
private func destroyTable(dictionary: Int): Void

private func roundUp(value: Int, alignment: Int): Int {
    return (value + alignment - 1) / alignment * alignment
}
)";

// The interleaved entry a literal packs: its ordinary struct layout is the runtime's entry layout.
inline constexpr auto tableEntry = R"(
@compiler_role(dict_entry)
private struct TableEntry<K, V> {
    var key: K
    var value: V
}
)";

inline constexpr auto tableHead = R"(
@compiler_role(dict_type)
public struct Table<K, V> {
    private var address: Int
    private var count: Int
    private var capacity: Int
)";

inline constexpr auto tableElement = R"(
    @compiler_role(dict_element)
    private borrowing func element(key: K): Int {
        return valueAt(data: self.address, count: self.count,
                key: joyeer_intrinsic_borrow_address<K>(value: key), keySize: joyeer_intrinsic_size_of<K>(),
                keyKind: joyeer_intrinsic_key_kind<K>() as Int32)
    }
)";

inline constexpr auto tableSet = R"(
    @compiler_role(dict_set)
    private mutating func insert(key: consuming K, value: consuming V): Void {
        let keySlot = joyeer_intrinsic_stack_slot<K>()
        let valueSlot = joyeer_intrinsic_stack_slot<V>()
        joyeer_intrinsic_move_in<K>(address: keySlot, value: consume key)
        joyeer_intrinsic_move_in<V>(address: valueSlot, value: consume value)
        setOwned(dictionary: joyeer_intrinsic_place_address<Table<K, V>>(value: &self), key: keySlot,
                value: valueSlot)
    }
)";

inline constexpr auto tableGet = R"(
    @compiler_role(dict_get)
    public borrowing func get(key: K): Optional<V> {
        let value = findValue(data: self.address, count: self.count,
                key: joyeer_intrinsic_borrow_address<K>(value: key), keySize: joyeer_intrinsic_size_of<K>(),
                keyKind: joyeer_intrinsic_key_kind<K>() as Int32)
        if value == 0 {
            return .None
        }
        let slot = joyeer_intrinsic_stack_slot<V>()
        joyeer_intrinsic_clone_at<V>(destination: slot, source: value)
        return .Some(joyeer_intrinsic_move_out<V>(address: slot))
    }
)";

inline constexpr auto tableCopy = R"(
    @compiler_role(dict_copy)
    private borrowing func duplicate(): Table<K, V> {
        let slot = joyeer_intrinsic_stack_slot<Table<K, V>>()
        cloneTable(output: slot, data: self.address, count: self.count)
        return joyeer_intrinsic_move_out<Table<K, V>>(address: slot)
    }
)";

// Called on an inert zero value with `count` entries packed at `address` (the closed packing protocol).
inline constexpr auto tableLiteral = R"(
    @compiler_role(dict_literal)
    private consuming func literal(address: Int, count: Int): Table<K, V> {
        let valueOffset = roundUp(value: joyeer_intrinsic_size_of<K>(), alignment: joyeer_intrinsic_align_of<V>())
        let slot = joyeer_intrinsic_stack_slot<Table<K, V>>()
        createTable(output: slot, entries: address, count: count, keySize: joyeer_intrinsic_size_of<K>(),
                valueSize: joyeer_intrinsic_size_of<V>(), entrySize: joyeer_intrinsic_size_of<TableEntry<K, V>>(),
                valueOffset: valueOffset, keyKind: joyeer_intrinsic_key_kind<K>() as Int32,
                cloneKey: joyeer_intrinsic_clone_callback<K>(), destroyKey: joyeer_intrinsic_destroy_callback<K>(),
                cloneValue: joyeer_intrinsic_clone_callback<V>(),
                destroyValue: joyeer_intrinsic_destroy_callback<V>())
        return joyeer_intrinsic_move_out<Table<K, V>>(address: slot)
    }
)";

inline constexpr auto tableTail = R"(
    deinit() {
        destroyTable(dictionary: joyeer_intrinsic_place_address<Table<K, V>>(value: &self))
    }
}
)";

// The whole table library: the externs, the entry and the dictionary role struct.
inline std::string dictRoleLibrary() {
    return std::string(tableExterns) + tableEntry + tableHead + tableElement + tableSet + tableGet + tableCopy +
           tableLiteral + tableTail;
}

} // namespace joyeer::fixtures

#endif
