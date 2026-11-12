#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/designware_i2c.h"
#include "hw/intc/sifive_plic.h"
#include "hw/intc/riscv_aclint.h"
#include "hw/riscv/riscv_hart.h"
#include "hw/riscv/boot.h"              /* riscv_plic_hart_config_string() */
#include "hw/char/serial-mm.h"
#include "hw/misc/soctest_timer.h"
#include "hw/misc/soctest_dma.h"
#include "hw/misc/sifive_test.h"
#include "system/memory.h"
#include "system/system.h"
#include "system/address-spaces.h"
#include "target/riscv/cpu.h"

#define SOCTEST_TEST_BASE    0x00100000
#define SOCTEST_CLINT_BASE   0x02000000 
#define SOCTEST_PLIC_BASE    0x0c000000
#define SOCTEST_UART0_BASE   0x10000000
#define SOCTEST_TIMER_BASE   0x10001000
#define SOCTEST_I2C_BASE     0x10002000
#define SOCTEST_DMA_BASE     0x10004000
#define SOCTEST_DDR_BASE     0x80000000

#define SOCTEST_DDR_SIZE            (2 * GiB)
#define SOCTEST_PLIC_SIZE           0x04000000

#define SOCTEST_PLIC_PRIORITY_BASE  0x00000000
#define SOCTEST_PLIC_PENDING_BASE   0x00001000
#define SOCTEST_PLIC_ENABLE_BASE    0x00002000
#define SOCTEST_PLIC_ENABLE_STRIDE  0x00000080
#define SOCTEST_PLIC_CONTEXT_BASE   0x00200000
#define SOCTEST_PLIC_CONTEXT_STRIDE 0x00001000

#define SOCTEST_PLIC_NUM_SOURCES    32
#define SOCTEST_PLIC_NUM_PRIO       7

#define SOCTEST_IRQ_UART0    1
#define SOCTEST_IRQ_TIMER    2
#define SOCTEST_IRQ_I2C      3
#define SOCTEST_IRQ_DMA    4

#define SOCTEST_I2C_SLAVE_ADDR  0x50

struct SocTestMachineState {
    MachineState parent_obj;
    RISCVHartArrayState harts;
    SocTestTimerState timer;
    DeviceState *plic;
    DesignWareI2CState i2c;
    SocTestDMAState dma;
};

#define TYPE_SOC_TEST_MACHINE MACHINE_TYPE_NAME("soctest")
OBJECT_DECLARE_SIMPLE_TYPE(SocTestMachineState, SOC_TEST_MACHINE)

// 初始化实例
static void soctest_machine_init(MachineState *machine){
    SocTestMachineState *s = SOC_TEST_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();

    object_initialize_child(OBJECT(machine), "harts", &s->harts, TYPE_RISCV_HART_ARRAY);
    qdev_prop_set_uint32(DEVICE(&s->harts), "num-harts", machine->smp.cpus);
    qdev_prop_set_uint32(DEVICE(&s->harts), "hartid-base", 0);
    qdev_prop_set_string(DEVICE(&s->harts), "cpu-type", machine->cpu_type);
    qdev_prop_set_uint64(DEVICE(&s->harts), "resetvec", SOCTEST_DDR_BASE);
    sysbus_realize(SYS_BUS_DEVICE(&s->harts), &error_fatal);

    memory_region_add_subregion(system_memory, SOCTEST_DDR_BASE, machine->ram);

    // 初始化ACLINT
    riscv_aclint_swi_create(system_memory, SOCTEST_CLINT_BASE, 
                            0, machine->smp.cpus, 
                            false);   /* sswi:false = M 模式 MSIP */
    riscv_aclint_mtimer_create(system_memory,
                               SOCTEST_CLINT_BASE + 0x4000,
                               RISCV_ACLINT_DEFAULT_MTIMER_SIZE,
                               0, machine->smp.cpus,
                               RISCV_ACLINT_DEFAULT_MTIMECMP,
                               RISCV_ACLINT_DEFAULT_MTIME, 
                               RISCV_ACLINT_DEFAULT_TIMEBASE_FREQ,
                               true);                                               

    // 初始化PLIC
    g_autofree char *plic_hart_config = riscv_plic_hart_config_string(machine->smp.cpus);
    s->plic = sifive_plic_create(system_memory, SOCTEST_PLIC_BASE, 
                                 plic_hart_config,
                                 machine->smp.cpus,
                                 0,
                                 SOCTEST_PLIC_NUM_SOURCES,
                                 SOCTEST_PLIC_NUM_PRIO,
                                 SOCTEST_PLIC_PRIORITY_BASE,
                                 SOCTEST_PLIC_PENDING_BASE,
                                 SOCTEST_PLIC_ENABLE_BASE,
                                 SOCTEST_PLIC_ENABLE_STRIDE,
                                 SOCTEST_PLIC_CONTEXT_BASE,
                                 SOCTEST_PLIC_CONTEXT_STRIDE,
                                 SOCTEST_PLIC_SIZE);

    /* SiFive Test finisher:guest 写 0x5555/0x3333/0x7777 → 关机 / panic / 复位 */
    sifive_test_create(SOCTEST_TEST_BASE);

    // 初始化DMA
    object_initialize_child(OBJECT(machine), "dma", &s->dma, TYPE_SOCTEST_DMA);
    object_property_set_link(OBJECT(&s->dma), "dma-memory",
                             OBJECT(get_system_memory()), &error_fatal);
    sysbus_realize(SYS_BUS_DEVICE(&s->dma), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->dma), 0, SOCTEST_DMA_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->dma), 0, qdev_get_gpio_in(s->plic, SOCTEST_IRQ_DMA));                         
                                 
    // 初始化UART0, 便捷写法
    serial_mm_init(system_memory, SOCTEST_UART0_BASE, 2, 
                   qdev_get_gpio_in(s->plic, SOCTEST_IRQ_UART0), 
                   115200*16, serial_hd(0), DEVICE_LITTLE_ENDIAN);

    // 初始化timer
    object_initialize_child(OBJECT(machine), "timer", &s->timer, TYPE_SOCTEST_TIMER);
    sysbus_realize(SYS_BUS_DEVICE(&s->timer), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->timer), 0, SOCTEST_TIMER_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->timer), 0, 
                       qdev_get_gpio_in(s->plic, SOCTEST_IRQ_TIMER));
    
    // 初始化I2C controller
    object_initialize_child(OBJECT(machine), "i2c", &s->i2c, TYPE_DESIGNWARE_I2C);
    sysbus_realize(SYS_BUS_DEVICE(&s->i2c), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->i2c), 0, SOCTEST_I2C_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->i2c), 0, 
                      qdev_get_gpio_in(s->plic, SOCTEST_IRQ_I2C));

    // 初始化I2C从设备
    // i2c_slave_create_simple(I2C_BUS(qdev_get_child_bus(DEVICE(&s->i2c), "i2c-bus")),
    //                         "i2c-echo",
    //                         SOCTEST_I2C_SLAVE_ADDR);
    // 显式创建与挂载
    DeviceState *i2c_dev = qdev_new("i2c-echo");
    object_property_add_child(OBJECT(&s->i2c), "i2c-echo", OBJECT(i2c_dev));     
    qdev_prop_set_uint8(i2c_dev, "address", SOCTEST_I2C_SLAVE_ADDR);
    qdev_realize_and_unref(i2c_dev, 
                           qdev_get_child_bus(DEVICE(&s->i2c), "i2c-bus"), 
                           &error_fatal);

}

// 初始化类
static void soctest_machine_class_init(ObjectClass *oc, const void *data){
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "socTest RISC-V machine (one hart, 2GiB DDR, UART/TIMER/PLIC)";
    mc->init = soctest_machine_init;
    mc->default_cpu_type = TYPE_RISCV_CPU_BASE;
    mc->max_cpus = 1;
    mc->default_ram_size = SOCTEST_DDR_SIZE;
    mc->default_ram_id = "riscv.soctest.ram";
}

// 初始化typeinfo
static const TypeInfo soctest_machine_typeinfo[] = {
    {
        .name = TYPE_SOC_TEST_MACHINE,
        .parent = TYPE_MACHINE,
        .instance_size = sizeof(SocTestMachineState),
        .class_init = soctest_machine_class_init,
    },
};

DEFINE_TYPES(soctest_machine_typeinfo)


