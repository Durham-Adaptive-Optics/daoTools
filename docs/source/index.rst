.. raw:: html

  <div style="background: linear-gradient(135deg, #68246D 0%, #00AEEF 100%); color: white; padding: 60px 40px; margin-bottom: 40px; border-radius: 8px;">
     <div style="display: flex; align-items: center; gap: 30px; flex-wrap: wrap;">
       <div>
         <h1 style="color: white; font-size: 2.8em; margin: 0 0 10px 0;">daoTools</h1>
         <p style="font-size: 1.2em; margin: 0 0 20px 0; opacity: 0.9;">DAO RTC Tools Library — process management, real-time controllers, data acquisition, and GUI utilities for adaptive optics systems.</p>
         <div style="display: flex; gap: 12px; flex-wrap: wrap;">
           <span style="background: rgba(255,255,255,0.15); padding: 6px 14px; border-radius: 20px; font-size: 0.9em;">Python Controllers</span>
           <span style="background: rgba(255,255,255,0.15); padding: 6px 14px; border-radius: 20px; font-size: 0.9em;">C Library</span>
           <span style="background: rgba(255,255,255,0.15); padding: 6px 14px; border-radius: 20px; font-size: 0.9em;">DAQ Toolchain</span>
           <span style="background: rgba(255,255,255,0.15); padding: 6px 14px; border-radius: 20px; font-size: 0.9em;">PyQt5 GUIs</span>
         </div>
       </div>
     </div>
   </div>

   <div style="display: grid; grid-template-columns: repeat(auto-fit, minmax(260px, 1fr)); gap: 20px; margin-bottom: 40px;">

     <a href="c_library.html" style="text-decoration: none;">
       <div style="border: 1px solid #e0e0e0; border-radius: 8px; padding: 24px; transition: box-shadow 0.2s; cursor: pointer;" onmouseover="this.style.boxShadow='0 4px 12px rgba(104,36,109,0.15)'" onmouseout="this.style.boxShadow='none'">
         <div style="font-size: 2em; margin-bottom: 12px;">🔧</div>
         <h3 style="color: #68246D; margin: 0 0 8px 0;">C Library</h3>
         <p style="color: #666; margin: 0; font-size: 0.9em;">Core C/C++ utilities: SHM helpers, checksum, IP tools, filter history, and the daoProfile instrumentation header.</p>
       </div>
     </a>

     <a href="python_api.html" style="text-decoration: none;">
       <div style="border: 1px solid #e0e0e0; border-radius: 8px; padding: 24px; transition: box-shadow 0.2s; cursor: pointer;" onmouseover="this.style.boxShadow='0 4px 12px rgba(104,36,109,0.15)'" onmouseout="this.style.boxShadow='none'">
         <div style="font-size: 2em; margin-bottom: 12px;">🐍</div>
         <h3 style="color: #68246D; margin: 0 0 8px 0;">Python API</h3>
         <p style="color: #666; margin: 0; font-size: 0.9em;">Core Python utilities: FIFO circular buffer, FITS I/O helpers, ZMQ wrappers, SHM recording, and remote SHM file client.</p>
       </div>
     </a>

     <a href="daolaunch.html" style="text-decoration: none;">
       <div style="border: 1px solid #e0e0e0; border-radius: 8px; padding: 24px; transition: box-shadow 0.2s; cursor: pointer;" onmouseover="this.style.boxShadow='0 4px 12px rgba(104,36,109,0.15)'" onmouseout="this.style.boxShadow='none'">
         <div style="font-size: 2em; margin-bottom: 12px;">🚀</div>
         <h3 style="color: #68246D; margin: 0 0 8px 0;">Process Management</h3>
         <p style="color: #666; margin: 0; font-size: 0.9em;">daoLaunch: tmux-based process lifecycle management — launch, kill, and health-check RTC processes over SSH.</p>
       </div>
     </a>

     <a href="process_controllers.html" style="text-decoration: none;">
       <div style="border: 1px solid #e0e0e0; border-radius: 8px; padding: 24px; transition: box-shadow 0.2s; cursor: pointer;" onmouseover="this.style.boxShadow='0 4px 12px rgba(104,36,109,0.15)'" onmouseout="this.style.boxShadow='none'">
         <div style="font-size: 2em; margin-bottom: 12px;">⚙️</div>
         <h3 style="color: #68246D; margin: 0 0 8px 0;">Process Controllers</h3>
         <p style="color: #666; margin: 0; font-size: 0.9em;">SHM-backed controller classes: centroid, MVM, loop, DM combiner, pixel calibration, clock — all built on ProcessController base.</p>
       </div>
     </a>

     <a href="command_interface.html" style="text-decoration: none;">
       <div style="border: 1px solid #e0e0e0; border-radius: 8px; padding: 24px; transition: box-shadow 0.2s; cursor: pointer;" onmouseover="this.style.boxShadow='0 4px 12px rgba(104,36,109,0.15)'" onmouseout="this.style.boxShadow='none'">
         <div style="font-size: 2em; margin-bottom: 12px;">📡</div>
         <h3 style="color: #68246D; margin: 0 0 8px 0;">Command Interface</h3>
         <p style="color: #666; margin: 0; font-size: 0.9em;">daoCommandIfce: ZMQ REQ/REP protobuf command client with timeout handling for inter-process communication.</p>
       </div>
     </a>

     <a href="apps.html" style="text-decoration: none;">
       <div style="border: 1px solid #e0e0e0; border-radius: 8px; padding: 24px; transition: box-shadow 0.2s; cursor: pointer;" onmouseover="this.style.boxShadow='0 4px 12px rgba(104,36,109,0.15)'" onmouseout="this.style.boxShadow='none'">
         <div style="font-size: 2em; margin-bottom: 12px;">🛠️</div>
         <h3 style="color: #68246D; margin: 0 0 8px 0;">Applications</h3>
         <p style="color: #666; margin: 0; font-size: 0.9em;">Command-line tools: centroid computation, MVM, pixel calibration, SHM monitoring, turbulence simulation, FITS conversion, and more.</p>
       </div>
     </a>

     <a href="gui.html" style="text-decoration: none;">
       <div style="border: 1px solid #e0e0e0; border-radius: 8px; padding: 24px; transition: box-shadow 0.2s; cursor: pointer;" onmouseover="this.style.boxShadow='0 4px 12px rgba(104,36,109,0.15)'" onmouseout="this.style.boxShadow='none'">
         <div style="font-size: 2em; margin-bottom: 12px;">🖥️</div>
         <h3 style="color: #68246D; margin: 0 0 8px 0;">GUI Tools</h3>
         <p style="color: #666; margin: 0; font-size: 0.9em;">PyQt5 GUI applications: SHM viewer with DAQ integration, DM display, image display, WFS slopes, RTD, log monitor, and remote SHM viewer.</p>
       </div>
     </a>

     <a href="daoDAQ.html" style="text-decoration: none;">
       <div style="border: 1px solid #e0e0e0; border-radius: 8px; padding: 24px; transition: box-shadow 0.2s; cursor: pointer;" onmouseover="this.style.boxShadow='0 4px 12px rgba(104,36,109,0.15)'" onmouseout="this.style.boxShadow='none'">
         <div style="font-size: 2em; margin-bottom: 12px;">💾</div>
         <h3 style="color: #68246D; margin: 0 0 8px 0;">daoDAQ</h3>
         <p style="color: #666; margin: 0; font-size: 0.9em;">Data acquisition tool: YAML-configured real-time recording of SHM streams and files to FITS with rollover and CPU affinity control.</p>
       </div>
     </a>

     <a href="daoDAQParser.html" style="text-decoration: none;">
       <div style="border: 1px solid #e0e0e0; border-radius: 8px; padding: 24px; transition: box-shadow 0.2s; cursor: pointer;" onmouseover="this.style.boxShadow='0 4px 12px rgba(104,36,109,0.15)'" onmouseout="this.style.boxShadow='none'">
         <div style="font-size: 2em; margin-bottom: 12px;">📊</div>
         <h3 style="color: #68246D; margin: 0 0 8px 0;">daoDAQ Parser</h3>
         <p style="color: #666; margin: 0; font-size: 0.9em;">Python library to parse daoDAQ FITS output: session loading, per-resource image arrays, timestamps, counters, and multi-resource sync.</p>
       </div>
     </a>

   </div>

daoTools Documentation
======================

.. toctree::
   :maxdepth: 2
   :caption: Core Libraries

   c_library
   python_api
   daolaunch

.. toctree::
   :maxdepth: 2
   :caption: Controllers & Interfaces

   process_controllers
   command_interface

.. toctree::
   :maxdepth: 2
   :caption: Applications & GUIs

   apps
   gui

.. toctree::
   :maxdepth: 2
   :caption: Data Acquisition

   daoDAQ
   daoDAQParser
