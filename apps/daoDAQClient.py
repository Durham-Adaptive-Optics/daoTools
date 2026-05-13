#!/usr/bin/env python3

'''
 # @ Author: Thomas N. Davies
 # @ Company: Centre for Advanced Instrumentation, Durham University
 # @ Contact: thomas.n.davies@durham.ac.uk
 # @ Create Time: 2026-05-07 10:13:30
 # @ Description: Dao DAQ Client Library and CLI Wrapper.
 '''

from daoCommandIfce import daoCommandIfce as daoAPI
from daoLog import daoLog
from enum import Enum
import time

DEFAULT_TCP_PORT: int = 62_000

""" DAQ Client States """
class DAQState(Enum):
    Unconfigured: int = 0
    Configured: int = 1
    Ready: int = 2
    Acquiring: int = 3
    Error: int = 4

""" DAQ Client Library 

    Provides means for connecting and communicating with a Dao DAQ instance. 
"""
class DAQClient:
    def __init__(self, daq_host_addr: str = "127.0.0.1", daq_host_port: int = DEFAULT_TCP_PORT, timeout_s=0.25):
        daoLog(__file__, toScreen=False)
        self.api = daoAPI(daq_host_addr, daq_host_port, timeout=timeout_s)

    def ping(self):
        status, _ = self.api.Ping()
        if status != 0:
            raise RuntimeError("failed to ping DAQ endpoint")

    def state(self) -> DAQState:
        status, stateName = self.api.State(None)
        if status != 0:
            raise RuntimeError("failed to retrieve DAQ state")
        
        stateMap = {
            "Off": DAQState.Unconfigured,
            "Standby": DAQState.Configured,
            "Idle": DAQState.Ready,
            "Running": DAQState.Acquiring,
            "Error": DAQState.Error
        }
        
        if stateName not in stateMap:
            raise RuntimeError(f"un-mapped DAQ state ({stateName})")
        
        return stateMap.get(stateName)
    
    def recover(self):
        if self.state() != DAQState.Error:
            raise RuntimeError("No recovery needed")
           
        status, _ = self.api.Exec("Recover")
        if status != 0 or self.state() != DAQState.Ready:
            raise RuntimeError("failed to recover DAQ server")
    
    def daq_session_configure_upload(self, daq_config: str):
        if self.state() not in (DAQState.Unconfigured, DAQState.Error):
            raise RuntimeError("DAQ will not accept new configuration in current state")
        
        status, _ = self.api.Other(daq_config)
        if status != 0:
            raise RuntimeError("failed to upload DAQ configuration")
        
    def daq_session_configure_apply(self):
        if self.state() != DAQState.Unconfigured:
            raise RuntimeError("DAQ not accepting new configurations")
        
        status, _ = self.api.Exec("Init")
        if status != 0 or self.state() != DAQState.Configured:
            raise RuntimeError("failed to apply DAQ configuration")
            
        status, _ = self.api.Exec("Enable")
        if status != 0 or self.state() != DAQState.Ready:
            raise RuntimeError("failed to preapre DAQ resources")
        
    def daq_session_begin(self):
        if self.state() != DAQState.Ready:
            raise RuntimeError("DAQ not ready to carry out a session")
           
        # note: We don't check if the tool reports as running — the session may finish before we check.
        status, _ = self.api.Exec("Run")
        if status != 0:
            raise RuntimeError("failed to begin DAQ session")

    def daq_session_await_finish(self, timeout=None, delay_s = 0.5):
        if self.state() != DAQState.Acquiring:
            raise RuntimeError("No DAQ session in progess")
           
        t0 = time.perf_counter()
        while True:
            elapsed_time = time.perf_counter() - t0
            if (timeout != None) and (elapsed_time >= timeout):
                raise RuntimeError("Timeout reached while awaiting DAQ session to auto-finish")
            
            if self.state() == DAQState.Ready:
                break
            
            if delay_s != None:
                time.sleep(delay_s)
        
    def daq_session_finish(self):
        if self.state() != DAQState.Acquiring:
            raise RuntimeError("No DAQ session in progess")
           
        status, _ = self.api.Exec("Idle")
        if status != 0 or self.state() != DAQState.Ready:
            raise RuntimeError("failed to finish DAQ session")
        
""" CLI Utility """
if __name__ == "__main__":
    import click

    def cli_error(msg: str):
        click.echo(
            click.style(msg, fg="red", bold=True)
        )
        raise click.exceptions.Exit(1)
    
    @click.group()
    @click.option('--host', default='127.0.0.1')
    @click.option('--port', default=DEFAULT_TCP_PORT, type=int)
    @click.pass_context
    def cli(ctx, host, port):
        ctx.ensure_object(dict)
        ctx.obj['daq_host'] = host
        ctx.obj['daq_port'] = port
    
    @cli.command()
    @click.pass_obj
    def ping(obj):
        """ Attempt to ping a DAQ server instance"""
        try:
            client = DAQClient(obj['daq_host'], obj['daq_port'])
            client.ping()
        except Exception as e:
            cli_error(e)
        else:
            click.echo(click.style("DAQ server ping successful", fg='green'))
    
    @cli.command()
    @click.pass_obj
    def state(obj):
        """ Retrieve DAQ Server State """
        try:
            client = DAQClient(obj['daq_host'], obj['daq_port'])
            state = client.state()
            echoCol = "red" if state == DAQState.Error else None
            click.echo(
                click.style(f"DAQ State: ") + 
                click.style(state.name, fg=echoCol, bold=True)
            )
        except Exception as e:
            cli_error(e)

    @cli.command()
    @click.pass_obj
    def recover(obj):
        """ Attempt to recover a DAQ server instance"""
        try:
            client = DAQClient(obj['daq_host'], obj['daq_port'])
            client.recover()
            click.echo(click.style("DAQ server recovery successful", fg='green'))
            click.echo(
                click.style(f"DAQ State: ") + 
                click.style(client.state().name, bold=True)
            )
        except Exception as e:
            cli_error(e)
        
    @cli.command()
    @click.pass_obj
    @click.argument('daq_config_path')
    def upload(obj, daq_config_path: str):
        """ Upload new DAQ configuration """
        try:
            with open(daq_config_path, "r") as daq_config_file:
                client = DAQClient(obj['daq_host'], obj['daq_port'])
                daq_config = daq_config_file.read()
                client.daq_session_configure_upload(daq_config)
                click.echo(click.style("DAQ configuration successfully uploaded", fg="green"))
        except Exception as e:
            cli_error(e)
    
    @cli.command()
    @click.pass_obj
    def apply(obj):
        """ Apply active DAQ configuration """
        try:
            client = DAQClient(obj['daq_host'], obj['daq_port'])
            client.daq_session_configure_apply()
            click.echo(click.style("DAQ configuration successfully applied", fg="green"))
        except Exception as e:
            cli_error(e)
    
    @cli.command()
    @click.pass_obj
    @click.option('--wait', is_flag=True, help='Block until DAQ session completes (session must be auto-finishable)')
    def acquire(obj, wait: bool):
        """ Begin DAQ session """
        try:
            client = DAQClient(obj['daq_host'], obj['daq_port'])
            client.daq_session_begin()
            click.echo(click.style("DAQ session successfully started", fg="green"))
            if wait:
                client.daq_session_await_finish()
        except Exception as e:
            cli_error(e)
    
    @cli.command()
    @click.pass_obj
    def wait(obj):
        """ Block until DAQ session finishes automatically (if applicable)"""
        try:
            client = DAQClient(obj['daq_host'], obj['daq_port'])
            client.daq_session_await_finish()
            click.echo(click.style("DAQ session finished", fg="green"))
        except Exception as e:
            cli_error(e)
    
    @cli.command()
    @click.pass_obj
    def finish(obj):
        """ Finish DAQ session """
        try:
            client = DAQClient(obj['daq_host'], obj['daq_port'])
            client.daq_session_finish()
            click.echo(click.style("DAQ session successfully finished", fg="green"))
        except Exception as e:
            cli_error(e)
    
    cli()
    