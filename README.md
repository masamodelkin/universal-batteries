# universal-batteries

Are you tired that all of those "wireless" devices actually require wires to charge them? Are you tired of all those wires on your table? Do you want to build a DIY project but you don't know what batteries to use? Or maybe you are not sure if wireless is the solution due to inconveniences with charging? 

If you answered yes to any of that, this project is for you! I personally answered yes to all of them, so I decided to make this modular battery system, so they can be swapped in seconds and no wires will be needed to charge any of the wireless tech that I have (not all of them, but most of them). 

This project introduces a simple design of a battery shell, charger and module that can be inserted into any project (or even existing electronics) to make it support hot-swap batteries and make all project batteries compatible with each other. 

The design features a charging station to charge 3 batteries at a time controlled with an ESP32-C6 allowing wireless control over it, battery shell design supporting 503759 batteries (1200mAh), and a battery bay to integrate into any project. 

This project is a work in progress, none of the electronics have been tested yet, including firmware and PCB. 

<img src="images/charger-render.png" alt="Charger render" width="500">

Here is the routed PCB for the charger:

<img src="images/routed-pcb.png" alt="Complete routed PCB layout" width="500">

## Bill of Materials (BOM)

| Component | Quantity | Unit Price | Total Price | Link |
|-----------|----------|------------|-------------|------|
| PCB (assembled) | 1 | 55.38 USD (35.38 + 20 shipping) | 55.38 USD | - |
| XIAO ESP32-C6 | 1 | 15.63 CAD (8.19 + 7.01 shipping + 0.43 antenna) | 15.63 CAD | [AliExpress](https://www.aliexpress.com/item/1005006935181127.html) |
| JST XH Connector Set (2-pin x3, 3-pin x1, 4-pin x1) | 1 | 6.92 CAD | 6.92 CAD | [AliExpress](https://www.aliexpress.com/item/1005009383841966.html) |
| Battery 503759 (1200mAh) | 3 | 14.94 CAD (pack of 3) | 14.94 CAD | [AliExpress](https://www.aliexpress.com/item/1005007102975858.html) |
| M3x6 Screws | 18 | - | - | - |
| M3x4x5 Heat Inserts | 18 | 7.55 CAD | 7.55 CAD | [AliExpress](https://www.aliexpress.com/item/1005009193787820.html) |
| Pogo Pins Connector 1x4 Male 4mm | 6 | 3.32 CAD | 19.92 CAD | [AliExpress](https://www.aliexpress.com/item/1005005380466642.html) |
| Pogo Pins Connector 1x4 Female 4mm | 6 | 3.48 CAD | 20.88 CAD | [AliExpress](https://www.aliexpress.com/item/1005005380466642.html) |
| Magnets 6x3mm | 18 | 11.24 CAD (4.33 + 6.91 shipping) | 11.24 CAD | [AliExpress](https://www.aliexpress.com/item/1005009258751295.html) |
| Magnets 6x2mm | 4 | 12.09 CAD (2.89 + 9.20 shipping) | 12.09 CAD | [AliExpress](https://www.aliexpress.com/item/1005009461555694.html) |
| PLA Filament | ~150g | Already own | 0.00 | - |
| **TOTAL** | | | **134.42 USD** | |

*Note: Prices include shipping where applicable. Some components may be sold in larger quantities than needed.*


