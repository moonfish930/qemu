#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
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
#include "hw/riscv/fdt-common.h"
#include "hw/char/serial-mm.h"
#include "hw/misc/soctest_timer.h"
#include "hw/misc/soctest_dma.h"
#include "hw/misc/sifive_test.h"
#include "system/memory.h"
#include "system/system.h"
#include "system/address-spaces.h"
#include "system/device_tree.h"
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
#define SOCTEST_CLINT_SIZE          0x00010000
#define SOCTEST_UART0_SIZE          0x00001000    // 理论0x20，对齐需求


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

// 给自定义设备建一个"标准 MMIO + 中断"节点
static void soctest_fdt_add_mmio_node(void *fdt, const char *name,
                                      hwaddr base, hwaddr size,
                                      const char *compatible,
                                      uint32_t plic_phandle, uint32_t irq)
{
    g_autofree char *path = g_strdup_printf("/soc/%s@%" HWADDR_PRIx,
                                            name, base);
    qemu_fdt_add_subnode(fdt, path);
    qemu_fdt_setprop_string(fdt, path, "compatible", compatible);
    qemu_fdt_setprop_sized_cells(fdt, path, "reg", 2, base, 2, size);
    
    if(irq){
        qemu_fdt_setprop_cell(fdt, path, "interrupt-parent", plic_phandle);
        qemu_fdt_setprop_cell(fdt, path, "interrupts", irq);
    }
}

static void soctest_create_fdt(SocTestMachineState *s){
    MachineState *ms = MACHINE(s);
    g_autofree uint32_t *intc_phandles = NULL;
    g_autofree uint32_t *plic_cells = NULL;
    uint32_t phandle = 1;
    uint32_t plic_phandle;
    uint32_t cells_len;
    int fdt_size;
    int i;

    /* 1) 树骨架:创建 "/" 和 "/soc"(ranges、2/2 cells) */
    ms->fdt = riscv_create_board_device_tree("socTest RISC-V machine",
                                             "soctest,soc-test", &fdt_size);
    /* 2) /cpus + /cpus/cpu-map,并写 timebase-frequency */
    riscv_fdt_create_cpu_socket_subnode(ms->fdt,
                                        RISCV_ACLINT_DEFAULT_TIMEBASE_FREQ);
    /* 3) 每个 hart 的 cpu@N 和 cpu-intc,顺便得到 intc_phandles[] */
    intc_phandles = g_new0(uint32_t, ms->smp.cpus);
    riscv_create_fdt_socket_cpus(ms->fdt, s->harts.harts, 0, ms->smp.cpus, 0,
                                 &phandle, intc_phandles,
                                 false,                       /* numa */
                                 riscv_is_32bit(&s->harts));
    /* 4) /memory@80000000(用 ram_size,尊重 -m) */
    riscv_create_fdt_socket_memory(ms->fdt, SOCTEST_DDR_BASE,
                                   ms->ram_size, 0, false);                                                                 
    /* 5) /soc/clint@2000000(MSIP=IRQ_M_SOFT=3, MTIP=IRQ_M_TIMER=7) */
    riscv_create_fdt_socket_clint(ms->fdt, SOCTEST_CLINT_BASE,
                                  SOCTEST_CLINT_SIZE, 0, intc_phandles,
                                  ms->smp.cpus, false);    
    /* 6) /soc/interrupt-controller@c000000(每 hart 两根线:M_EXT/S_EXT) */
    cells_len = ms->smp.cpus * 4;
    plic_cells = g_new0(uint32_t, cells_len);
    for (i = 0; i < ms->smp.cpus; i++) {
        plic_cells[i * 4 + 0] = cpu_to_be32(intc_phandles[i]);
        plic_cells[i * 4 + 1] = cpu_to_be32(IRQ_M_EXT);
        plic_cells[i * 4 + 2] = cpu_to_be32(intc_phandles[i]);
        plic_cells[i * 4 + 3] = cpu_to_be32(IRQ_S_EXT);
    }
    plic_phandle = phandle++;
    riscv_create_fdt_plic(ms->fdt, SOCTEST_PLIC_BASE, SOCTEST_PLIC_SIZE,
                          plic_phandle,
                          FDT_PLIC_INT_CELLS, FDT_PLIC_ADDR_CELLS,
                          plic_cells, cells_len * sizeof(uint32_t),
                          SOCTEST_PLIC_NUM_SOURCES - 1,  /* riscv,ndev */
                          false, 0);
    /* 7) UART:helper 建节点(compatible=ns16550a),但**不设 reg-shift**,
     *    我们的 UART 是 regshift=2,必须补上,否则 Linux 读寄存器全错位。*/
    {
        MemMapEntry uart_mem = { SOCTEST_UART0_BASE, SOCTEST_UART0_SIZE };
        g_autofree char *uart_name = riscv_fdt_get_uart_nodename(uart_mem.base);
        riscv_create_fdt_uart(ms->fdt, &uart_mem, SOCTEST_IRQ_UART0, AIA_TYPE_NONE, plic_phandle);
        qemu_fdt_setprop_cell(ms->fdt, uart_name, "reg-shift", 2);
        /* 与 serial_mm_init() 的 baudbase 保持一致 */
        qemu_fdt_setprop_cell(ms->fdt, uart_name, "clock-frequency",
                              115200 * 16);
    }        
    /* 8) 自定义设备 */
    soctest_fdt_add_mmio_node(ms->fdt, "timer",
                              SOCTEST_TIMER_BASE, 0x1000,
                              "soctest,timer", plic_phandle,
                              SOCTEST_IRQ_TIMER);
    soctest_fdt_add_mmio_node(ms->fdt, "dma",
                              SOCTEST_DMA_BASE, 0x1000,
                              "soctest,dma", plic_phandle,
                              SOCTEST_IRQ_DMA);
    /* 9) I2C 控制器 + 总线上的从设备 */                                                                                  
    {
        g_autofree char *i2c = g_strdup_printf("/soc/i2c@%" HWADDR_PRIx,
                                               (hwaddr)SOCTEST_I2C_BASE);
        g_autofree char *echo = g_strdup_printf("%s/i2c-echo@%x", i2c,
                                               SOCTEST_I2C_SLAVE_ADDR);
        
        qemu_fdt_add_subnode(ms->fdt, i2c);
        qemu_fdt_setprop_string(ms->fdt, i2c, "compatible",
                                "snps,designware-i2c");
        qemu_fdt_setprop_sized_cells(ms->fdt, i2c, "reg",
                                     2, (hwaddr)SOCTEST_I2C_BASE,
                                     2, (hwaddr)0x1000);
        qemu_fdt_setprop_cell(ms->fdt, i2c, "#address-cells", 1);
        qemu_fdt_setprop_cell(ms->fdt, i2c, "#size-cells", 0);
        qemu_fdt_setprop_cell(ms->fdt, i2c, "interrupt-parent", plic_phandle);
        qemu_fdt_setprop_cell(ms->fdt, i2c, "interrupts", SOCTEST_IRQ_I2C);
        qemu_fdt_setprop_cell(ms->fdt, i2c, "clock-frequency", 100000);
        
        qemu_fdt_add_subnode(ms->fdt, echo);
        qemu_fdt_setprop_string(ms->fdt, echo, "compatible",
                                "soctest,i2c-echo");
        qemu_fdt_setprop_cell(ms->fdt, echo, "reg", SOCTEST_I2C_SLAVE_ADDR);
    }
    /* 10) finisher(syscon) + /reboot + /poweroff,值就是 0x7777 / 0x5555 */
    riscv_create_fdt_syscon(ms->fdt, &phandle, SOCTEST_TEST_BASE, 0x1000,
                            FINISHER_RESET, FINISHER_PASS,
                            true /* 加 "sifive,test1","sifive,test0" 兼容串 */);
    /* 11) /chosen:必须现在建好(以后 riscv_load_kernel 会往里写 bootargs) */
    qemu_fdt_add_subnode(ms->fdt, "/chosen");
    qemu_fdt_setprop_string(ms->fdt, "/chosen", "stdout-path",
                            "/soc/serial@10000000:115200n8");                        

}



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

    if(machine->dtb){
        int fdt_size = 0;

        machine->fdt = load_device_tree(machine->dtb, &fdt_size);
        if (!machine->fdt) {
            error_report("load_device_tree() failed: %s", machine->dtb);
            exit(EXIT_FAILURE);
        }
    } else {
        soctest_create_fdt(s);
    }                       
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


