#!/usr/bin/env python3
'''
 # @ Author: Thomas N. Davies
 # @ Company: Centre for Advanced Instrumentation, Durham University
 # @ Contact: thomas.n.davies@durham.ac.uk
 # @ Create Time: 2026-05-07 10:13:30
 # @ Description: Dao DAQ Client CLI Tool.
 '''

from daoDAQClient import *
import click

if __name__ == "__main__":
    def cli_error(msg: str):
        click.echo(
            click.style(msg, fg="red", bold=True)
        )
        raise click.exceptions.Exit(1)
    
    @click.group()
    @click.option('--host', default='127.0.0.1')
    @click.option('--port', default=DAQClient.DEFAULT_TCP_PORT, type=int)
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