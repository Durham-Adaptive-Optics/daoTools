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

    ''' Internal Methods '''
    def dao_invoke_(self, method: callable, *args):
        status, payload = method(*args)
        if status != 0:
            raise RuntimeError(f"{method.__name__}.status -> {status}")
        return payload
    
    ''' API Methods '''

    def ping(self):
        try:
            self.dao_invoke_(self.api.Ping)
        except Exception as e:
            raise RuntimeError(f"failed to ping DAQ endpoint ({e})")

    def state(self) -> DAQState:
        stateMap = {
            "Off": DAQState.Unconfigured,
            "Standby": DAQState.Configured,
            "Idle": DAQState.Ready,
            "Running": DAQState.Acquiring,
            "Error": DAQState.Error
        }

        try:
            stateName = self.dao_invoke_(self.api.State, None)
            if stateName not in stateMap:
                raise RuntimeError(f"un-mapped DAO API state {stateName}")
            return stateMap.get(stateName)
        except Exception as e:
            raise RuntimeError(f"failed to fetch DAQ tool state ({e})")
    
    def recover(self):
        try:
            if self.state() != DAQState.Error:
                raise RuntimeError("DAQ tool is already operational")
           
            self.dao_invoke_(self.api.Exec, "Recover")
            
            if self.state() != DAQState.Ready:
                raise RuntimeError()
        except Exception as e:
            raise RuntimeError(f"failed to recover DAQ tool {e}")
    
    def daq_session_configure_upload(self, daq_config: str):
        try:
            if self.state() is DAQState.Acquiring:
                raise RuntimeError("DAQ session in-progress")
            
            # move tool into the Off state before we can upload
            # new config.
            if self.state() is DAQState.Ready:
                self.dao_invoke_(self.api.Exec, "Disable")
                self.dao_invoke_(self.api.Exec, "Stop")
            elif self.state() is DAQState.Configured:
                self.dao_invoke_(self.api.Exec, "Stop")
            
            if self.state() not in (DAQState.Unconfigured, DAQState.Error):
                raise RuntimeError("DAQ tool not ready to accept new config")
            
            self.dao_invoke_(self.api.Other, daq_config)
        except Exception as e:
            raise RuntimeError(f"failed to upload DAQ config: {e}")
        
    def daq_session_configure_apply(self):
        try:
            if self.state() != DAQState.Unconfigured:
                raise RuntimeError("a DAQ config is currently applied")
            
            self.dao_invoke_(self.api.Exec, "Init")
            self.dao_invoke_(self.api.Exec, "Enable")

            if self.state() != DAQState.Ready:
                raise RuntimeError("")
        except Exception as e:
            raise RuntimeError(f"failed to apply DAQ config: {e}")
        
    def daq_session_begin(self):
        try:
            if self.state() != DAQState.Ready:
                raise RuntimeError("no DAQ resources available")
            
            self.dao_invoke_(self.api.Exec, "Run")
            
            # note: we don't check the state entered Running here as
            # the session in theory could finish before we check and
            # then we report an error when in-fact everything is fine.
        except Exception as e:
            raise RuntimeError(f"failed to begin DAQ session: {e}")
           
    def daq_session_await_finish(self, timeout=None, delay_s = 0.5):
        try:
            t0 = time.perf_counter()
            while True:
                elapsed_time = time.perf_counter() - t0
                if (timeout != None) and (elapsed_time >= timeout):
                    raise RuntimeError("client timeout")
                
                if self.state() == DAQState.Ready:
                    break
                
                if delay_s != None:
                    time.sleep(delay_s)
        except Exception as e:
            raise RuntimeError(f"failed to await DAQ session finish: {e}")
                
    def daq_session_finish(self):
        try: 
            if self.state() != DAQState.Acquiring:
                raise RuntimeError("no DAQ session in progess")
            
            self.dao_invoke_(self.api.Exec, "Idle")
            
            endState = self.state()
            if endState != DAQState.Ready:
                raise RuntimeError(f"end state is {endState} not {DAQState.Ready}")
        except Exception as e:
            raise RuntimeError(f"failed to finish DAQ session: {e}")
        
        
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
    