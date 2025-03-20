# Willo Kernel Keyboard Filter Driver

## Overview

Willo is a kernel-mode keyboard filter driver for Windows. It intercepts keyboard input to perform two main functions:

- **Keystroke Injection:** When one of the F1–F8 keys is pressed, the driver reads the corresponding text file (`F1.txt` to `F8.txt`) from the disk and injects the keystrokes from that file into the keyboard stream.
- **Dynamic File Reloading:** When the F9 key is pressed, the driver reloads all the text files. This allows updates to the text files without rebooting or reloading the driver manually.

> **Warning:** Kernel-mode drivers run at a very low level and can cause system instability or blue screens if not implemented or tested carefully.

## Suggested Environment

- **Virtual Machine (VM):** It is **highly recommended** to test this driver on a virtual machine (e.g., using Hyper-V) or on a secondary PC.
- **Avoid Testing on Your Main PC:** Kernel drivers, especially self-signed ones, can easily lead to blue screens (BSOD), so do not use your primary machine for testing.

## Prerequisites

- **Visual Studio:** Use Visual Studio for compiling the driver.
- **Windows Driver Kit (WDK):** Download and install the WDK for building Windows drivers.
- **Administrative Privileges:** Ensure you have administrative rights to install and start kernel drivers.

## Pre-Installation Steps

1. **Disable Secure Boot:**  
   In your BIOS settings, turn off Secure Boot. Secure Boot may prevent the loading of self-signed drivers.

2. **Enable Test Mode in Windows:**  
   Open an elevated Command Prompt and run:  
   ```cmd
   bcdedit.exe -set TESTSIGNING ON
   ```  
   This allows Windows to load self-signed drivers. You might need to restart your system for the changes to take effect.

> **Note:** Windows normally does not load self-signed drivers unless test mode is enabled.

## Building the Driver

1. **Open the Project:**  
   Open the solution in Visual Studio.

2. **Set Up the Environment:**  
   Make sure the WDK is properly configured in your Visual Studio environment.

3. **Build the Driver:**  
   Compile the project to generate the driver file (e.g., `WilloKernel.sys`).

## Installation

1. **Copy the Driver File:**  
   Move the generated `.sys` file to the Windows drivers directory:  
   ```cmd
   move /Y "Path\To\Your\WilloKernel.sys" "C:\Windows\System32\drivers\WilloKernel.sys"
   ```

2. **Create the Driver Service:**  
   Open an elevated Command Prompt and run the following command to create a service for the driver:
   ```cmd
   sc create WilloKernel type= kernel start= demand binPath= "C:\Windows\System32\drivers\WilloKernel.sys"
   ```

3. **Start the Driver:**  
   To load the driver into memory, execute:
   ```cmd
   sc start WilloKernel
   ```

## Usage

- **Keystroke Injection:**  
  Press one of the F1–F8 keys to trigger the injection of keystrokes from the corresponding text file located in `C:\WilloProject\copy_sources\`.

- **Reload Files:**  
  Press the F9 key to asynchronously reload all the text files. This allows you to update the file contents without unloading the driver.

## Stopping and Uninstalling

- **Stop the Driver:**  
  To stop the driver, run:
  ```cmd
  sc stop WilloKernel
  ```

- **Remove the Driver Service:**  
  If needed, you can delete the service using:
  ```cmd
  sc delete WilloKernel
  ```

## Final Notes

- **Testing Caution:**  
  As this is a kernel-mode driver, any bugs or issues may lead to system crashes. Always test in a safe environment, such as a virtual machine or a dedicated test system.
- **Self-Signed Drivers:**  
  Remember that running self-signed drivers requires test mode on Windows, and you should disable Secure Boot to load them.

Enjoy experimenting with Willo, and always ensure your testing environment is secure!
