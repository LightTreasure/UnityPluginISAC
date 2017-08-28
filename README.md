# UnityPluginISAC

## Prerequisites
You'll need to have the following installed in order to compile the plugin:

* Windows SDK version 10.0.15063.468 or higher
* Visual Studio 2017 (Visual Studio 2015 might work, but hasn't been tested)

In order to use and run the plugin, you will need:

* Unity version 5.2 or higher
* Windows 10 Creator's Update or higher

## Build instructions

The repository contains two Visual Studio Solutions in the VisualStudio directory:

* **AudioPluginMsHRTF.sln**: Use this solution to generate the Desktop (Win32) version of the plugin. Use it for the "PC, Mac & Linux Standalone" build target of Unity with the target platform of Windows.
* **AudioPluginMsHRTF_UWP.sln**: Use this solution to generate the UWP version of the plugin. Use it for the "Windows Store" build target of Unity.

### Desktop (Win32)

* Open AudioPluginMsHRTF.sln in Visual Studio. Right click on the solution name in Solution Explorer, and use "Retarget solution" to change the Windows SDK version to the one you have installed.
* Make sure the solution configuration is set to "Release" and the solution configuration is set to "Win32".
* Build the solution. The plugin will be generated in the "build\Release -DLL\" subdirectory of the "VisualStudio" directory. The name of the binary will be "AudioPluginMsHRTF.dll" and the symbols can be found in "AudioPluginMsHRTF.pdb".

### UWP

* Open AudioPluginMsHRTF_UWP.sln in Visual Studio. Right click on the solution name in Solution Explorer, and use "Retarget solution" to change the Windows SDK version to the one you have installed.
* Make sure the solution configuration is set to "Release" and the solution configuration is set to "x64" or "x86".
* Build the solution. The plugin will be generated in the "build\Release\" subdirectory of the "VisualStudio" directory. The name of the binary will be "AudioPluginMsHRTF_UWP.dll" and the symbols can be found in "AudioPluginMsHRTF_UWP.pdb".

## How to Use with Unity

* Once built, copy the plugin dll to your Unity project's "Assets\Plugins\" directory.
* If you're using the UWP version of the plugin, select it in the Unity Editor and in the Inspector pane make sure the only platform for the plugin is "WSAPlayer". Also in the "Platform settings" section in the Inspector pane, SDK should be set to "UWP", CPU is set to match the architecture the plugin was compiled with, and ScriptingBacked is set to "Any Scripting Backend". Click "Apply" to make these changes.
* For the Desktop version of the plugin, the platform should be "Standalone". In the Standalone setting section, make sure you select the CPU architecture that th plugin was built for. 
* Go to the Edit Menu -> Project Settings -> Audio and select "MS HRTF Spatializer" for the Spatializer Plugin setting.
* Audio Sources with the "Spatialize" checkbox checked will be rendered via the ISAC plugin. 
