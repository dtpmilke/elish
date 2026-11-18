/* elish: PEP-конфиг UART QUP0 SE6 (Bluetooth QCA6390) для \_SB.UR18 (DSDT v7u: MMIO 0x998000, IRQ 0x27F).
 * Сделан из MTP-конфига UR18 (QUP2 SE3, пины 43-46): такты -> wrap0_s6 / wrap_0_m_ahb, шины QUP_2 -> QUP_0,
 * пины -> GPIO16 CTS, 17 RTS, 18 TX, 19 RX (функция qup6). PSTATE 2..13 = частоты SE (индексы qcuart). */
DefinitionBlock ("", "SSDT", 2, "QCOMM ", "ELSHBT", 1)
{
    External (\_SB.PEP0, DeviceObj)
    External (\_SB.UR18, DeviceObj)
    Scope (\_SB.PEP0)
    {
        Method (BUMD, 0, NotSerialized)
        {
            Return (BURC)
        }

        Name (BURC, Package (One)
        {
                Package (0x04)
                {
                    "DEVICE", 
                    "\\_SB.UR18", 
                    Package (0x12)
                    {
                        "COMPONENT", 
                        Zero, 
                        Package (0x06)
                        {
                            "FSTATE", 
                            Zero, 
                            Package (0x02)
                            {
                                "BUSARB", 
                                Package (0x05)
                                {
                                    0x03, 
                                    "ICBID_MASTER_QUP_0", 
                                    "ICBID_SLAVE_EBI1", 
                                    0x0927C000, 
                                    0x0682
                                }
                            }, 

                            Package (0x02)
                            {
                                "BUSARB", 
                                Package (0x05)
                                {
                                    0x03, 
                                    "ICBID_MASTER_APPSS_PROC", 
                                    "ICBID_SLAVE_QUP_0", 
                                    0x08D24D00, 
                                    0x02FAF080
                                }
                            }, 

                            Package (0x02)
                            {
                                "CLOCK", 
                                Package (0x02)
                                {
                                    "gcc_qupv3_wrap_0_m_ahb_clk", 
                                    One
                                }
                            }, 

                            Package (0x02)
                            {
                                "CLOCK", 
                                Package (0x02)
                                {
                                    "gcc_qupv3_wrap0_s6_clk", 
                                    One
                                }
                            }
                        }, 

                        Package (0x06)
                        {
                            "FSTATE", 
                            One, 
                            Package (0x02)
                            {
                                "CLOCK", 
                                Package (0x02)
                                {
                                    "gcc_qupv3_wrap0_s6_clk", 
                                    0x02
                                }
                            }, 

                            Package (0x02)
                            {
                                "CLOCK", 
                                Package (0x02)
                                {
                                    "gcc_qupv3_wrap_0_m_ahb_clk", 
                                    0x02
                                }
                            }, 

                            Package (0x02)
                            {
                                "BUSARB", 
                                Package (0x05)
                                {
                                    0x03, 
                                    "ICBID_MASTER_APPSS_PROC", 
                                    "ICBID_SLAVE_QUP_0", 
                                    Zero, 
                                    Zero
                                }
                            }, 

                            Package (0x02)
                            {
                                "BUSARB", 
                                Package (0x05)
                                {
                                    0x03, 
                                    "ICBID_MASTER_QUP_0", 
                                    "ICBID_SLAVE_EBI1", 
                                    Zero, 
                                    Zero
                                }
                            }
                        }, 

                        Package (0x06)
                        {
                            "PSTATE", 
                            Zero, 
                            Package (0x02)
                            {
                                "TLMMGPIO", 
                                Package (0x06)
                                {
                                    0x10, 
                                    Zero, 
                                    One, 
                                    Zero, 
                                    One, 
                                    Zero
                                }
                            }, 

                            Package (0x02)
                            {
                                "TLMMGPIO", 
                                Package (0x06)
                                {
                                    0x11, 
                                    Zero, 
                                    One, 
                                    Zero, 
                                    Zero, 
                                    Zero
                                }
                            }, 

                            Package (0x02)
                            {
                                "TLMMGPIO", 
                                Package (0x06)
                                {
                                    0x12, 
                                    Zero, 
                                    One, 
                                    Zero, 
                                    Zero, 
                                    Zero
                                }
                            }, 

                            Package (0x02)
                            {
                                "TLMMGPIO", 
                                Package (0x06)
                                {
                                    0x13, 
                                    Zero, 
                                    One, 
                                    Zero, 
                                    One, 
                                    Zero
                                }
                            }
                        }, 

                        Package (0x06)
                        {
                            "PSTATE", 
                            One, 
                            Package (0x02)
                            {
                                "TLMMGPIO", 
                                Package (0x06)
                                {
                                    0x10, 
                                    Zero, 
                                    One, 
                                    Zero, 
                                    One, 
                                    Zero
                                }
                            }, 

                            Package (0x02)
                            {
                                "TLMMGPIO", 
                                Package (0x06)
                                {
                                    0x11, 
                                    Zero, 
                                    One, 
                                    Zero, 
                                    Zero, 
                                    Zero
                                }
                            }, 

                            Package (0x02)
                            {
                                "TLMMGPIO", 
                                Package (0x06)
                                {
                                    0x12, 
                                    Zero, 
                                    One, 
                                    Zero, 
                                    Zero, 
                                    Zero
                                }
                            }, 

                            Package (0x02)
                            {
                                "TLMMGPIO", 
                                Package (0x06)
                                {
                                    0x13, 
                                    Zero, 
                                    One, 
                                    Zero, 
                                    One, 
                                    Zero
                                }
                            }
                        }, 

                        Package (0x03)
                        {
                            "PSTATE", 
                            0x02, 
                            Package (0x02)
                            {
                                "CLOCK", 
                                Package (0x04)
                                {
                                    "gcc_qupv3_wrap0_s6_clk", 
                                    0x03, 
                                    0x00708000, 
                                    0x04
                                }
                            }
                        }, 

                        Package (0x03)
                        {
                            "PSTATE", 
                            0x03, 
                            Package (0x02)
                            {
                                "CLOCK", 
                                Package (0x04)
                                {
                                    "gcc_qupv3_wrap0_s6_clk", 
                                    0x03, 
                                    0x00E10000, 
                                    0x04
                                }
                            }
                        }, 

                        Package (0x03)
                        {
                            "PSTATE", 
                            0x04, 
                            Package (0x02)
                            {
                                "CLOCK", 
                                Package (0x04)
                                {
                                    "gcc_qupv3_wrap0_s6_clk", 
                                    0x03, 
                                    0x01C20000, 
                                    0x04
                                }
                            }
                        }, 

                        Package (0x03)
                        {
                            "PSTATE", 
                            0x05, 
                            Package (0x02)
                            {
                                "CLOCK", 
                                Package (0x04)
                                {
                                    "gcc_qupv3_wrap0_s6_clk", 
                                    0x03, 
                                    0x01E84800, 
                                    0x04
                                }
                            }
                        }, 

                        Package (0x03)
                        {
                            "PSTATE", 
                            0x06, 
                            Package (0x02)
                            {
                                "CLOCK", 
                                Package (0x04)
                                {
                                    "gcc_qupv3_wrap0_s6_clk", 
                                    0x03, 
                                    0x02DC6C00, 
                                    0x04
                                }
                            }
                        }, 

                        Package (0x03)
                        {
                            "PSTATE", 
                            0x07, 
                            Package (0x02)
                            {
                                "CLOCK", 
                                Package (0x04)
                                {
                                    "gcc_qupv3_wrap0_s6_clk", 
                                    0x03, 
                                    0x03D09000, 
                                    0x04
                                }
                            }
                        }, 

                        Package (0x03)
                        {
                            "PSTATE", 
                            0x08, 
                            Package (0x02)
                            {
                                "CLOCK", 
                                Package (0x04)
                                {
                                    "gcc_qupv3_wrap0_s6_clk", 
                                    0x03, 
                                    0x04C4B400, 
                                    0x04
                                }
                            }
                        }, 

                        Package (0x03)
                        {
                            "PSTATE", 
                            0x09, 
                            Package (0x02)
                            {
                                "CLOCK", 
                                Package (0x04)
                                {
                                    "gcc_qupv3_wrap0_s6_clk", 
                                    0x03, 
                                    0x05B8D800, 
                                    0x04
                                }
                            }
                        }, 

                        Package (0x03)
                        {
                            "PSTATE", 
                            0x0A, 
                            Package (0x02)
                            {
                                "CLOCK", 
                                Package (0x04)
                                {
                                    "gcc_qupv3_wrap0_s6_clk", 
                                    0x03, 
                                    0x061A8000, 
                                    0x04
                                }
                            }
                        }, 

                        Package (0x03)
                        {
                            "PSTATE", 
                            0x0B, 
                            Package (0x02)
                            {
                                "CLOCK", 
                                Package (0x04)
                                {
                                    "gcc_qupv3_wrap0_s6_clk", 
                                    0x03, 
                                    0x06ACFC00, 
                                    0x04
                                }
                            }
                        }, 

                        Package (0x03)
                        {
                            "PSTATE", 
                            0x0C, 
                            Package (0x02)
                            {
                                "CLOCK", 
                                Package (0x04)
                                {
                                    "gcc_qupv3_wrap0_s6_clk", 
                                    0x03, 
                                    0x07080000, 
                                    0x04
                                }
                            }
                        }, 

                        Package (0x03)
                        {
                            "PSTATE", 
                            0x0D, 
                            Package (0x02)
                            {
                                "CLOCK", 
                                Package (0x04)
                                {
                                    "gcc_qupv3_wrap0_s6_clk", 
                                    0x03, 
                                    0x07A12000, 
                                    0x04
                                }
                            }
                        }
                    }, 

                    Package (0x04)
                    {
                        "COMPONENT", 
                        One, 
                        Package (0x02)
                        {
                            "FSTATE", 
                            Zero
                        }, 

                        Package (0x02)
                        {
                            "FSTATE", 
                            One
                        }
                    }
                }
        })
    }
}
