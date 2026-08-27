"""
AS/RS Prototype AI Service (Gradio version for Hugging Face Spaces)
- Handles store/retrieve commands from Unity via Firebase
- Runs D-EAO to optimise storage layout (frequency + weight penalty)
- Manages inventory, items, and event history in Firebase
"""
import os
import json
import numpy as np
import gradio as gr
from datetime import datetime

import firebase_admin
from firebase_admin import credentials, db

# ── Firebase init ──
DATABASE_URL = "https://asmr-3dd8a-default-rtdb.asia-southeast1.firebasedatabase.app"

if not firebase_admin._apps:
    try:
        cred_json = os.environ.get("FIREBASE_CREDENTIALS_JSON", None)
        if cred_json:
            cred = credentials.Certificate(json.loads(cred_json))
        else:
            cred = credentials.Certificate("serviceAccountKey.json")
        firebase_admin.initialize_app(cred, {"databaseURL": DATABASE_URL})
    except Exception as e:
        print(f"WARNING: Firebase init failed: {e}")

# ── Rack geometry (from Arduino Mega step positions) ──
SLOTS = ["A1", "A2", "A3", "B1", "B2", "B3", "C1", "C2", "C3"]

TRAVEL_STEPS = {
    "A1": 79950, "A2": 66700, "A3": 54450,
    "B1": 65950, "B2": 52900, "B3": 40450,
    "C1": 51250, "C2": 38200, "C3": 25750,
}
max_steps = max(TRAVEL_STEPS.values())
TRAVEL_COST = {s: v / max_steps for s, v in TRAVEL_STEPS.items()}

# ── Weight penalty parameter (matches simulation Section 3.6) ──
LAMBDA_W = 1.0


# ═══════════════════════════════════════════════════════════════
# FIREBASE HELPERS
# ═══════════════════════════════════════════════════════════════

def get_event_history():
    data = db.reference("/ASRS/EventHistory").get()
    if not data:
        return []
    if isinstance(data, list):
        return [e for e in data if e is not None]
    events = []
    for key in sorted(data.keys(), key=lambda k: int(k)):
        events.append(data[key])
    return events


def append_event(item, operation, slot):
    ref = db.reference("/ASRS/EventHistory")
    data = ref.get()
    if isinstance(data, list):
        next_key = str(len(data))
    elif data:
        next_key = str(max(int(k) for k in data.keys()) + 1)
    else:
        next_key = "1"
    ref.child(next_key).set({
        "item": item,
        "operation": operation,
        "slot": slot,
        "timestamp": datetime.now().isoformat()
    })


def get_inventory_raw():
    """Get raw inventory data from Firebase (with timestamps)."""
    return db.reference("/ASRS/Inventory").get() or {s: "" for s in SLOTS}


def get_inventory():
    """Get inventory as {slot: item_name} for simple lookups."""
    raw = get_inventory_raw()
    result = {}
    for s in SLOTS:
        val = raw.get(s, "")
        if isinstance(val, dict):
            result[s] = val.get("item", "")
        else:
            result[s] = val if val else ""
    return result


def set_inventory_slot(slot, item):
    if item:
        db.reference(f"/ASRS/Inventory/{slot}").set({
            "item": item,
            "stored_at": datetime.now().isoformat()
        })
    else:
        db.reference(f"/ASRS/Inventory/{slot}").set("")


def get_layout():
    return db.reference("/ASRS/Layout").get() or {}


def set_layout(layout):
    db.reference("/ASRS/Layout").set(layout)


def set_ai_command(command):
    db.reference("/ASRS/AICommand").set(command)


def set_status(status):
    db.reference("/ASRS/Status").set(status)


def get_items():
    """Get all registered items from Firebase. Returns {name: weight}."""
    data = db.reference("/ASRS/Items").get()
    if not data:
        return {}
    return {name: float(info.get("weight", 1.0)) for name, info in data.items()}


def add_item_to_db(name, weight):
    """Register a new item in Firebase."""
    db.reference(f"/ASRS/Items/{name}").set({"weight": weight})


def get_item_names():
    """Get list of registered item names."""
    items = get_items()
    return sorted(items.keys()) if items else []


# ═══════════════════════════════════════════════════════════════
# D-EAO OPTIMISER (with weight penalty, λ_w = 1.0)
# ═══════════════════════════════════════════════════════════════

def compute_item_frequencies(event_history, item_list):
    freq = {item: 0 for item in item_list}
    for event in event_history:
        item = event["item"]
        if item in freq:
            freq[item] += 1
    return freq


def proxy_fitness(permutation, frequencies, item_list, item_weights):
    """
    Proxy fitness: f = Σ freq_i × cost_j × (1 + λ_w × w_i)
    Heavier + more frequent items are penalised more for distant slots.
    Identical formulation to the simulation (Section 3.6, λ_w = 1.0).
    """
    total = 0.0
    for item_idx, slot_idx in enumerate(permutation):
        item = item_list[item_idx]
        freq = frequencies.get(item, 0)
        weight = item_weights.get(item, 1.0)
        cost = TRAVEL_COST[SLOTS[slot_idx]]
        total += freq * cost * (1.0 + LAMBDA_W * weight)
    return total


def replay_fitness(permutation, event_history, item_list, item_weights):
    """
    Replay fitness: simulates the event sequence and accumulates
    cost × (1 + λ_w × w) for each operation.
    """
    item_to_slot = {}
    for item_idx, slot_idx in enumerate(permutation):
        item_to_slot[item_list[item_idx]] = SLOTS[slot_idx]

    inventory = {s: [] for s in SLOTS}
    total_cost = 0.0

    for event in event_history:
        item = event["item"]
        op = event["operation"]
        weight = item_weights.get(item, 1.0)

        if op == "store":
            preferred = item_to_slot.get(item, SLOTS[0])
            if not inventory[preferred]:
                slot_used = preferred
            else:
                slot_used = None
                best_cost = float("inf")
                for s in SLOTS:
                    if not inventory[s]:
                        cost = TRAVEL_STEPS[s]
                        if cost < best_cost:
                            best_cost = cost
                            slot_used = s
                if slot_used is None:
                    continue
            inventory[slot_used].append(item)
            total_cost += TRAVEL_COST[slot_used] * (1.0 + LAMBDA_W * weight)
        else:
            oldest_slot = None
            for s in SLOTS:
                if item in inventory[s]:
                    oldest_slot = s
                    break
            if oldest_slot:
                inventory[oldest_slot].remove(item)
                total_cost += TRAVEL_COST[oldest_slot] * (1.0 + LAMBDA_W * weight)

    return total_cost


def move_toward(perm, target, n_steps, rng):
    result = perm.copy()
    inv = np.zeros_like(result)
    for i, v in enumerate(result):
        inv[v] = i
    disagree = np.where(result != target)[0]
    if len(disagree) == 0 or n_steps == 0:
        return result
    n_fix = min(n_steps, len(disagree))
    chosen = rng.choice(disagree, size=n_fix, replace=False)
    for pos in chosen:
        want = target[pos]
        cur_pos_of_want = inv[want]
        displaced = result[pos]
        result[pos], result[cur_pos_of_want] = want, displaced
        inv[want] = pos
        inv[displaced] = cur_pos_of_want
    return result


def transfer_structure(perm, source, rng):
    result = perm.copy()
    inv = np.zeros_like(result)
    for i, v in enumerate(result):
        inv[v] = i
    diff_positions = np.where(source != perm)[0]
    if len(diff_positions) == 0:
        return result
    for pos in diff_positions:
        want = source[pos]
        cur_pos = inv[want]
        displaced = result[pos]
        result[pos], result[cur_pos] = want, displaced
        inv[want] = pos
        inv[displaced] = cur_pos
    return result


def random_perturb(perm, n_swaps, rng):
    result = perm.copy()
    n = len(result)
    for _ in range(n_swaps):
        i, j = rng.choice(n, size=2, replace=False)
        result[i], result[j] = result[j], result[i]
    return result


def run_deao(event_history, item_list, item_weights, use_replay=True, seed=42):
    N = len(item_list)
    rng = np.random.default_rng(seed)
    frequencies = compute_item_frequencies(event_history, item_list)

    if use_replay:
        def fitness(perm):
            return replay_fitness(perm, event_history, item_list, item_weights)
    else:
        def fitness(perm):
            return proxy_fitness(perm, frequencies, item_list, item_weights)

    POP_SIZE = 50
    MAX_ITER = 50
    PATIENCE = 15

    population = []
    for _ in range(POP_SIZE):
        perm = rng.choice(len(SLOTS), size=N, replace=False).astype(np.int32)
        population.append(perm)

    fit_values = np.array([fitness(p) for p in population])
    best_idx = np.argmin(fit_values)
    global_best = population[best_idx].copy()
    global_best_fit = fit_values[best_idx]
    stagnation = 0

    for iteration in range(MAX_ITER):
        AF = (iteration + 1) / MAX_ITER
        new_population = []
        new_fits = []

        for i in range(POP_SIZE):
            substrate = population[i]
            rho1, rho2 = rng.random(), rng.random()

            hamming = np.sum(substrate != global_best)
            n_fix = max(1, int(np.ceil(hamming * AF * rho1)))
            cand1 = move_toward(substrate, global_best, n_fix, rng)
            n_perturb = max(0, int(np.floor((1 - AF) * rho2 * N * 0.3)))
            if n_perturb > 0:
                cand1 = random_perturb(cand1, n_perturb, rng)

            partner = population[rng.integers(POP_SIZE)]
            cand2 = transfer_structure(substrate, partner, rng)
            n_fix2 = max(1, int(np.ceil(np.sum(cand2 != global_best) * AF * rho1 * 0.5)))
            cand2 = move_toward(cand2, global_best, n_fix2, rng)

            f1, f2, f_old = fitness(cand1), fitness(cand2), fit_values[i]

            if f1 <= f2 and f1 <= f_old:
                new_population.append(cand1); new_fits.append(f1)
            elif f2 <= f_old:
                new_population.append(cand2); new_fits.append(f2)
            else:
                new_population.append(substrate); new_fits.append(f_old)

        population = new_population
        fit_values = np.array(new_fits)
        current_best_idx = np.argmin(fit_values)
        if fit_values[current_best_idx] < global_best_fit:
            global_best = population[current_best_idx].copy()
            global_best_fit = fit_values[current_best_idx]
            stagnation = 0
        else:
            stagnation += 1
        if stagnation >= PATIENCE:
            break

    return global_best, global_best_fit


# ═══════════════════════════════════════════════════════════════
# GRADIO INTERFACE FUNCTIONS
# ═══════════════════════════════════════════════════════════════

def register_item(name, weight):
    """Register a new item with its weight."""
    name = name.strip()
    if not name:
        return "Error: Item name cannot be empty", gr.update()
    if weight is None or weight <= 0:
        return "Error: Weight must be a positive number", gr.update()

    existing = get_items()
    if name in existing:
        return f"Error: Item '{name}' already exists (weight: {existing[name]} kg)", gr.update()

    add_item_to_db(name, weight)
    item_names = get_item_names()
    return f"Registered '{name}' with weight {weight} kg", gr.update(choices=item_names)


def store_item(item):
    """Store an item — called from Unity or Gradio UI."""
    items = get_items()
    if not item or item not in items:
        return f"Error: Unknown item '{item}'. Register it first in the Items tab."

    layout = get_layout()
    inventory = get_inventory()

    target_slot = layout.get(item)

    if target_slot and inventory.get(target_slot, "") == "":
        slot = target_slot
    else:
        # Fallback: score empty slots by travel_cost × (1 + λ_w × weight)
        # Heavier items get placed in closer (cheaper) slots
        weight = items.get(item, 1.0)
        slot = None
        best_score = float("inf")
        for s in SLOTS:
            if inventory.get(s, "") == "":
                score = TRAVEL_COST[s] * (1.0 + LAMBDA_W * weight)
                if score < best_score:
                    best_score = score
                    slot = s

    if slot is None:
        return "Error: Rack is full, no empty slots"

    set_inventory_slot(slot, item)
    append_event(item, "store", slot)
    command = f"{slot}_RETURN"
    set_ai_command(command)

    return f"Storing {item} ({items[item]} kg) in slot {slot} | Command sent: {command}"


def retrieve_item(item):
    """Retrieve an item — FIFO using stored_at timestamp."""
    items = get_items()
    if not item or item not in items:
        return f"Error: Unknown item '{item}'. Register it first in the Items tab."

    raw = get_inventory_raw()
    # Find all slots holding this item, with their stored_at timestamp
    item_slots = []
    for s in SLOTS:
        val = raw.get(s, "")
        if isinstance(val, dict) and val.get("item") == item:
            item_slots.append((s, val.get("stored_at", "9999")))
        elif val == item:
            # Legacy format without timestamp — treat as oldest
            item_slots.append((s, "0000"))

    if not item_slots:
        return f"Error: No {item} in inventory"

    # FIFO: pick the slot with the earliest stored_at timestamp
    item_slots.sort(key=lambda x: x[1])
    slot = item_slots[0][0]

    set_inventory_slot(slot, "")
    append_event(item, "retrieve", slot)
    command = f"{slot}_TAKE"
    set_ai_command(command)

    return f"Retrieving {item} ({items[item]} kg) from slot {slot} | Command sent: {command}"


def reoptimise():
    """Re-run D-EAO on accumulated event history."""
    set_status("OPTIMISING")
    event_history = get_event_history()

    if len(event_history) < 5:
        set_status("IDLE")
        return "Error: Not enough events to optimise (need at least 5)"

    items = get_items()
    item_list = sorted(items.keys())

    if len(item_list) == 0:
        set_status("IDLE")
        return "Error: No items registered. Add items in the Items tab first."

    # Rank items by frequency — only top 9 (num slots) get layout assignments
    frequencies = compute_item_frequencies(event_history, item_list)
    ranked = sorted(item_list, key=lambda x: frequencies.get(x, 0), reverse=True)
    layout_items = ranked[:len(SLOTS)]  # top items get dedicated slots
    overflow_items = ranked[len(SLOTS):]  # rest use weight-aware fallback

    best_perm, best_fit = run_deao(event_history, layout_items, items, use_replay=True)

    layout = {}
    for item_idx, slot_idx in enumerate(best_perm):
        layout[layout_items[item_idx]] = SLOTS[slot_idx]

    set_layout(layout)
    set_status("IDLE")

    result = "Layout re-optimised! (D-EAO with weight penalty, lambda_w=1.0)\n\n"
    result += "Item → Slot (weight, frequency):\n"
    for item, slot in layout.items():
        w = items.get(item, 0)
        f = frequencies.get(item, 0)
        result += f"  {item} ({w} kg, freq={f}) → {slot} (cost: {TRAVEL_COST[slot]:.3f})\n"
    if overflow_items:
        result += f"\nItems without dedicated slot ({len(overflow_items)}):\n"
        for item in overflow_items:
            w = items.get(item, 0)
            f = frequencies.get(item, 0)
            result += f"  {item} ({w} kg, freq={f}) — uses weight-aware fallback\n"
    result += f"\nFitness: {best_fit:.4f}"
    result += f"\nEvents used: {len(event_history)}"
    result += f"\nItems in layout: {len(layout_items)}/{len(item_list)}"
    return result


def show_inventory():
    """Display current inventory."""
    inventory = get_inventory()
    items = get_items()
    result = "Current Inventory:\n\n"
    for slot in SLOTS:
        item = inventory.get(slot, "")
        if item:
            w = items.get(item, "?")
            result += f"  {slot}: {item} ({w} kg)\n"
        else:
            result += f"  {slot}: (empty)\n"
    occupied = sum(1 for v in inventory.values() if v)
    result += f"\nOccupied: {occupied}/9"
    return result


def show_layout():
    """Display current optimised layout."""
    layout = get_layout()
    items = get_items()
    if not layout:
        return "No layout computed yet. Click 'Re-optimise' first."
    result = "Current Optimised Layout:\n\n"
    for item, slot in layout.items():
        w = items.get(item, "?")
        result += f"  {item} ({w} kg) → {slot}\n"
    return result


def show_items():
    """Display all registered items."""
    items = get_items()
    if not items:
        return "No items registered yet. Add items in the form above."
    result = "Registered Items:\n\n"
    for name in sorted(items.keys()):
        result += f"  {name}: {items[name]} kg\n"
    result += f"\nTotal: {len(items)} items"
    return result


def refresh_item_dropdown():
    """Refresh the item dropdown with current items from Firebase."""
    item_names = get_item_names()
    return gr.update(choices=item_names)


# ═══════════════════════════════════════════════════════════════
# GRADIO API (callable from Unity via HTTP POST)
# ═══════════════════════════════════════════════════════════════

def api_store(item):
    return store_item(item)

def api_retrieve(item):
    return retrieve_item(item)

def api_reoptimise():
    return reoptimise()

def api_inventory():
    return show_inventory()

def api_layout():
    return show_layout()

def api_register_item(name, weight):
    result, _ = register_item(name, weight)
    return result


# ═══════════════════════════════════════════════════════════════
# BUILD GRADIO UI + API
# ═══════════════════════════════════════════════════════════════

# Load initial items from Firebase for dropdown
try:
    initial_items = get_item_names()
except Exception:
    initial_items = []

with gr.Blocks(title="AS/RS AI Service") as demo:
    gr.Markdown("# AS/RS Prototype AI Service")
    gr.Markdown("D-EAO optimised storage layout with weight penalty (lambda_w=1.0)")

    with gr.Tab("Items"):
        gr.Markdown("### Register New Item")
        gr.Markdown("Add items before storing them. Each item needs a name and weight.")
        with gr.Row():
            item_name_input = gr.Textbox(label="Item Name", placeholder="e.g. Item_A")
            item_weight_input = gr.Number(label="Weight (kg)", value=1.0, minimum=0.01)
        register_btn = gr.Button("Register Item", variant="primary")
        register_result = gr.Textbox(label="Result", lines=2)

        gr.Markdown("### Registered Items")
        items_btn = gr.Button("Refresh Item List")
        items_display = gr.Textbox(label="Items", lines=8)
        items_btn.click(fn=show_items, inputs=None, outputs=items_display)

    with gr.Tab("Store / Retrieve"):
        with gr.Row():
            item_input = gr.Dropdown(choices=initial_items, label="Select Item")
            refresh_btn = gr.Button("Refresh", scale=0)
        with gr.Row():
            store_btn = gr.Button("Store", variant="primary")
            retrieve_btn = gr.Button("Retrieve", variant="secondary")
        result_text = gr.Textbox(label="Result", lines=3)

        refresh_btn.click(fn=refresh_item_dropdown, inputs=None, outputs=item_input)
        store_btn.click(fn=store_item, inputs=item_input, outputs=result_text)
        retrieve_btn.click(fn=retrieve_item, inputs=item_input, outputs=result_text)

    with gr.Tab("Optimise"):
        optimise_btn = gr.Button("Re-optimise Layout (D-EAO)", variant="primary")
        optimise_result = gr.Textbox(label="Result", lines=12)
        optimise_btn.click(fn=reoptimise, inputs=None, outputs=optimise_result)

    with gr.Tab("View"):
        with gr.Row():
            inv_btn = gr.Button("Show Inventory")
            layout_btn = gr.Button("Show Layout")
        view_result = gr.Textbox(label="Result", lines=12)
        inv_btn.click(fn=show_inventory, inputs=None, outputs=view_result)
        layout_btn.click(fn=show_layout, inputs=None, outputs=view_result)

    # Wire up register button — updates both result text and dropdown
    register_btn.click(fn=register_item, inputs=[item_name_input, item_weight_input],
                       outputs=[register_result, item_input])

# Launch with API enabled
# demo.launch(server_name="0.0.0.0", server_port=7860, share=False)
# demo.launch(server_name="0.0.0.0", server_port=7860, ssr_mode=False)
# demo.launch(server_name="0.0.0.0", server_port=int(os.environ.get("PORT", 7860)))
demo.launch(server_name="0.0.0.0", server_port=int(os.environ.get("PORT", 7860)), share=False)
# demo.launch(server_name="0.0.0.0", server_port=7860)