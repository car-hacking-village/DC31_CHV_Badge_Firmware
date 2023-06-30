
// Include MicroPython API.
#include <py/runtime.h>
#include <py/obj.h>
#include <py/gc.h>


#include <RP2040.h>
#include <pico/stdlib.h>
#include <hardware/pio.h>
#include <hardware/claim.h>
#include <can2040.h>

STATIC const mp_obj_type_t mp_type_caninterface;

// We have this wrapper object for the internals for the same reason micropython does
// We need to lie about it's type while smuggling additional variables in
typedef struct canbus_internal {
    struct can2040 internal;
    mp_obj_t * recv_queue;
    mp_fun_1_t pop;
    mp_fun_2_t push;
} canbus_internal_t;

typedef struct {
    mp_obj_base_t base;
    canbus_internal_t bus;
    uint32_t sys_clock;
    uint32_t bitrate;
    uint32_t gpio_rx;
    uint32_t gpio_tx;
    uint32_t pio_num;
    bool started;
} mp_obj_can_interface_t;

// PRIVATE
STATIC mp_obj_can_interface_t *mp_can_obj_0;
STATIC mp_obj_can_interface_t *mp_can_obj_1;

static void
can2040_cb(struct can2040 *cd, uint32_t notify, struct can2040_msg *msg)
{
    canbus_internal_t* bus = (canbus_internal_t*)cd; 
    if (notify == CAN2040_NOTIFY_RX)
    {
        // We shouldn't need to worry about packing because it is all multiples of 64.
        // So no matter the architecture it will be on cache lines
        // Also we probably don't need to care about an exception being thrown since 
        // we don't set the flags which would cause the exception
        bus->push(bus->recv_queue, mp_obj_new_bytes((byte*)msg, sizeof(struct can2040_msg)));
        mp_printf(MICROPY_ERROR_PRINTER, "RX!!!!\n");
    }
    if (notify & CAN2040_NOTIFY_ERROR)
    {
        mp_printf(MICROPY_ERROR_PRINTER, "Error...\n");
    }

}

static void can2040_internal_pio0_irq_handler(void)
{
    // lock the python internals while we run our irq to make sure we aren't interupted!
    mp_sched_lock();
    gc_lock();

    // handle the irq
    // TODO: how?
    can2040_pio_irq_handler(&(mp_can_obj_0->bus.internal));

    // unlock in reverse order!
    gc_unlock();
    mp_sched_unlock();
}

static void can2040_internal_pio1_irq_handler(void)
{
    // lock the python internals while we run our irq to make sure we aren't interupted!
    mp_sched_lock();
    gc_lock();

    // handle the irq
    // TODO: how?
    can2040_pio_irq_handler(&(mp_can_obj_1->bus.internal));

    // unlock in reverse order!
    gc_unlock();
    mp_sched_unlock();
}

STATIC mp_obj_t can_init_helper(mp_obj_can_interface_t *self ) {

    pio_hw_t * pio = (self->pio_num == 0) ? pio0_hw : pio1_hw;
    uint pio_irq = (self->pio_num == 0) ? PIO0_IRQ_0 : PIO1_IRQ_0;

    // Setup canbus internal structure
    can2040_setup(&self->bus.internal, self->pio_num);
    can2040_callback_config(&self->bus.internal, can2040_cb);

    // disable the irq while configuring it
    irq_set_enabled(pio_irq, false);

    // claim all the pio sm irqs
    for (uint8_t i=0; i < 4; i ++) {
        if (pio_sm_is_claimed(pio, i)) {
            irq_set_enabled(pio_irq, true); // reenable since we don't own it
            mp_raise_ValueError("StateMachine claimed by external resource!");
        }
        pio_sm_claim(pio, i);
    }


    irq_handler_t handler = irq_get_exclusive_handler(pio_irq);
    if (handler != NULL) {
        // this is the default MicroPython irq handler... hopefully
        irq_remove_handler(pio_irq, handler);
    }

    // configure with our IRQ handler
    
    irq_set_exclusive_handler(pio_irq, 
        (self->pio_num == 0) ? can2040_internal_pio0_irq_handler : can2040_internal_pio1_irq_handler);
    irq_set_priority(pio_irq, PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_enabled(pio_irq, true);

    // Start canbus
    can2040_start(&self->bus.internal, self->sys_clock, self->bitrate, self->gpio_rx, self->gpio_tx, 10);
    self->started = true;


    return MP_OBJ_FROM_PTR(self);
}

// STATIC mp_obj_t mp_can_init(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
//     mp_printf(MICROPY_ERROR_PRINTER, "in __init__\n");
//     return can_init_helper(MP_OBJ_TO_PTR(pos_args[0]), n_args - 1, pos_args + 1, kw_args);
// }
// STATIC MP_DEFINE_CONST_FUN_OBJ_KW(mp_can_init_obj, 1 , mp_can_init);

STATIC mp_obj_t mp_can_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    // parse args
    mp_map_t kw_args;
    mp_map_init_fixed_table(&kw_args, n_kw, all_args + n_args);
    enum { ARG_bitrate, ARG_sysclock, ARG_gpiorx, ARG_gpiotx, ARG_pionum};
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_bitrate,  MP_ARG_INT,   {.u_int = 500000} },
        { MP_QSTR_sysclock, MP_ARG_INT,   {.u_int = 125000000} },
        { MP_QSTR_gpiorx,   MP_ARG_INT,   {.u_int = 11} },
        { MP_QSTR_gpiotx,   MP_ARG_INT,   {.u_int = 12} },
        { MP_QSTR_pionum,   MP_ARG_INT,   {.u_int = 1}  },
    };

    // parse args
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, all_args, &kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    uint32_t pio_num = args[ARG_pionum].u_int;
    if ((pio_num != 0) && (pio_num != 1)) {
        mp_raise_ValueError("Invalid PIO number. Must be 0 or 1");
    }

    // setup the object
    mp_obj_can_interface_t *self;
    if ((pio_num == 0) && (mp_can_obj_0 != NULL)) {
        self = mp_can_obj_0;
    } else if ((pio_num == 1) && (mp_can_obj_1 != NULL)) {
        self = mp_can_obj_1;
    } else {
        // create a new obj and save it for later
        self = m_new_obj(mp_obj_can_interface_t);
        self->base.type = &mp_type_caninterface;
        if (pio_num == 0) {
            mp_can_obj_0 = self;
        } else {
            mp_can_obj_1 = self;
        }

        // create a new deque and call it's make_new (__init__)
        mp_obj_t deque_args[2] = {mp_const_empty_tuple, mp_obj_new_int(10)};
        self->bus.recv_queue = MP_OBJ_TYPE_GET_SLOT(&mp_type_deque, make_new)(&mp_type_deque, 2, 0, deque_args);

        // add direct function calls so that we don't need to do lookup every time we use them
        mp_map_t *locals_map = &MP_OBJ_TYPE_GET_SLOT(type, locals_dict)->map;
        self->bus.pop = (mp_fun_1_t)mp_map_lookup(locals_map, MP_ROM_QSTR(MP_QSTR_popleft), MP_MAP_LOOKUP);
        self->bus.push = (mp_fun_2_t)mp_map_lookup(locals_map, MP_ROM_QSTR(MP_QSTR_append), MP_MAP_LOOKUP);

        self->gpio_rx = args[ARG_gpiorx].u_int;
        self->gpio_tx = args[ARG_gpiotx].u_int;
        self->sys_clock = args[ARG_sysclock].u_int;
        self->bitrate = args[ARG_bitrate].u_int;
        self->pio_num = pio_num;

        if (pio_num == 0) {
            mp_can_obj_0 = self;
        } else {
            mp_can_obj_1 = self;
        }
        
        // Need to setup here and set the PIO interface
        can_init_helper(self);
    }

    return MP_OBJ_FROM_PTR(self);
}

STATIC mp_obj_t mp_can_send_helper(mp_obj_can_interface_t *self, size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_id, ARG_dlc, ARG_data, ARG_extframe};
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_id,       MP_ARG_REQUIRED | MP_ARG_INT,   {.u_int = 0} },
        { MP_QSTR_dlc,      MP_ARG_REQUIRED | MP_ARG_INT,   {.u_int = 0} },
        { MP_QSTR_data,     MP_ARG_REQUIRED | MP_ARG_OBJ,   {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_extframe, MP_ARG_BOOL,                    {.u_bool = false} },
    };

    // parse args
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (!self->started) {
        mp_raise_ValueError("Canbus is stopped");
    }

    struct can2040_msg response;

    if (args[ARG_extframe].u_bool == true) {
        response.id = args[ARG_id].u_int & 0x1FFFFFFF;
    } else {
        response.id = args[ARG_id].u_int & 0x7FF;
    }

    response.dlc = args[ARG_dlc].u_int & 0xFF;

    mp_buffer_info_t data;
    mp_get_buffer_raise(args[ARG_data].u_obj, &data, MP_BUFFER_READ);
    for(int i = 0; i < data.len || i < 8; i++) {
        response.data[i] = ((uint8_t*)data.buf)[i];
    }

    // Debugging
    // mp_printf(MICROPY_ERROR_PRINTER, "%d - %d\n", response.id, response.dlc);
    // for(int i = 0; i < 8; i++) {
    //     mp_printf(MICROPY_ERROR_PRINTER, "%d\n", ((uint8_t*)data.buf)[i]);
    // }

    can2040_transmit(&self->bus.internal, &response);

    return mp_const_none;
}
STATIC mp_obj_t mp_can_send(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    return mp_can_send_helper(MP_OBJ_TO_PTR(pos_args[0]), n_args - 1, pos_args + 1, kw_args);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(canbus_send_obj, 1 , mp_can_send);

STATIC mp_obj_t mp_can_recv_helper(mp_obj_can_interface_t *self, size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {

    struct can2040_msg msg;

    if (!self->started) {
        mp_raise_ValueError("Canbus is stopped");
    }

    mp_obj_t res = mp_call_function_1_protected(self->bus.pop,self->bus.recv_queue);
    if (res == MP_OBJ_NULL) {
        return MP_OBJ_NULL; // TODO: Do we want to keep waiting instead?
    }
    
    mp_obj_t *items;

    mp_obj_t ret_obj = mp_obj_new_tuple(3, NULL);
    items = ((mp_obj_tuple_t *)MP_OBJ_TO_PTR(ret_obj))->items;
    items[2] = mp_obj_new_bytes(msg.data, 8);

    items[0] = MP_OBJ_NEW_SMALL_INT(msg.id);
    items[1] = MP_OBJ_NEW_SMALL_INT(msg.dlc);

    // Return the result
    return ret_obj;
}
STATIC mp_obj_t mp_can_recv(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    return mp_can_recv_helper(MP_OBJ_TO_PTR(pos_args[0]), n_args - 1, pos_args + 1, kw_args);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(canbus_recv_obj, 1 , mp_can_recv);

STATIC mp_obj_t mp_can_start(mp_obj_t self_in) {
    mp_obj_can_interface_t*self = MP_OBJ_TO_PTR(self_in);

    if (!self->started) {
        can2040_start(&(self->bus.internal), self->sys_clock, self->bitrate, self->gpio_rx, self->gpio_tx, 10);
        self->started = true;
    }

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(canbus_start_obj, mp_can_start);

STATIC mp_obj_t mp_can_stop(mp_obj_t self_in) {
    mp_obj_can_interface_t*self = MP_OBJ_TO_PTR(self_in);

    if (self->started) {
        can2040_stop(&(self->bus.internal));
        self->started = false;
    }

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(canbus_stop_obj, mp_can_stop);

STATIC mp_obj_t propertyclass_bitrate(mp_obj_t self_in) {
    mp_obj_can_interface_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_int(self->bitrate);
}

MP_DEFINE_CONST_FUN_OBJ_1(propertyclass_bitrate_obj, propertyclass_bitrate);

STATIC mp_obj_t propertyclass_gpio_rx(mp_obj_t self_in) {
    mp_obj_can_interface_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_int(self->gpio_rx);
}

MP_DEFINE_CONST_FUN_OBJ_1(propertyclass_gpio_rx_obj, propertyclass_gpio_rx);

STATIC mp_obj_t propertyclass_gpio_tx(mp_obj_t self_in) {
    mp_obj_can_interface_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_int(self->gpio_tx);
}

MP_DEFINE_CONST_FUN_OBJ_1(propertyclass_gpio_tx_obj, propertyclass_gpio_tx);

STATIC mp_obj_t propertyclass_pio_num(mp_obj_t self_in) {
    mp_obj_can_interface_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_int(self->pio_num);
}

MP_DEFINE_CONST_FUN_OBJ_1(propertyclass_pio_num_obj, propertyclass_pio_num);

STATIC mp_obj_t propertyclass_started(mp_obj_t self_in) {
    mp_obj_can_interface_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(self->started);
}

MP_DEFINE_CONST_FUN_OBJ_1(propertyclass_started_obj, propertyclass_started);

STATIC mp_obj_t propertyclass_state(mp_obj_t self_in) {
    mp_obj_can_interface_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_int(self->bus.internal.report_state);
}

MP_DEFINE_CONST_FUN_OBJ_1(propertyclass_state_obj, propertyclass_state);


// TODO: figure out the properties part
// STATIC void propertyclass_attr(mp_obj_t self, qstr attribute, mp_obj_t *destination) {

//     if(attribute == MP_QSTR_bitrate) {
//         destination[0] = propertyclass_bitrate(self);
//     } else if (attribute == MP_QSTR_gpiorx) {
//         destination[0] = propertyclass_gpio_rx(self);
//     } else if (attribute == MP_QSTR_gpiotx) {
//         destination[0] = propertyclass_gpio_tx(self);
//     } else if (attribute == MP_QSTR_pionum) {
//         destination[0] = propertyclass_pio_num(self);
//     } else if (attribute == MP_QSTR_started) {
//         destination[0] = propertyclass_started(self);
//     } else if (attribute == MP_QSTR_send) {
//         destination[0] = &canbus_send_obj;
//     }
// }


STATIC const mp_rom_map_elem_t canhack_caninterface_locals_dict_table[] = {
    // { MP_ROM_QSTR(MP_QSTR___init__), MP_ROM_PTR(&mp_can_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&canbus_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_recv), MP_ROM_PTR(&canbus_recv_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&canbus_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&canbus_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_bitrate), MP_ROM_PTR(&propertyclass_bitrate_obj) },
    { MP_ROM_QSTR(MP_QSTR_gpiorx), MP_ROM_PTR(&propertyclass_gpio_rx_obj) },
    { MP_ROM_QSTR(MP_QSTR_gpiotx), MP_ROM_PTR(&propertyclass_gpio_tx_obj) },
    { MP_ROM_QSTR(MP_QSTR_pionum), MP_ROM_PTR(&propertyclass_pio_num_obj) },
    { MP_ROM_QSTR(MP_QSTR_started), MP_ROM_PTR(&propertyclass_started_obj) },
    { MP_ROM_QSTR(MP_QSTR_state), MP_ROM_PTR(&propertyclass_state_obj) },
};
STATIC MP_DEFINE_CONST_DICT(canhack_caninterface_locals_dict, canhack_caninterface_locals_dict_table);

STATIC MP_DEFINE_CONST_OBJ_TYPE(
    mp_type_caninterface,
    MP_QSTR_canbus,
    MP_TYPE_FLAG_NONE,
    make_new, mp_can_make_new,
    // attr, propertyclass_attr,
    locals_dict, &canhack_caninterface_locals_dict
    );

STATIC const mp_rom_map_elem_t can_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_canbus) },
    { MP_ROM_QSTR(MP_QSTR_bus), MP_ROM_PTR(&mp_type_caninterface) },
};
STATIC MP_DEFINE_CONST_DICT(can_module_globals, can_module_globals_table);

const mp_obj_module_t mp_module_canhack = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&can_module_globals,
};

// Register the module 'can' and make it available in Python
MP_REGISTER_MODULE(MP_QSTR_canbus, mp_module_canhack);
