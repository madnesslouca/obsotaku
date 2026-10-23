#!/usr/bin/python3
"""Private protocol fixture, never a product backend. No secret values are logged."""
import argparse
import os
from pathlib import Path
import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

P = "/org/freedesktop/secrets"
COLLECTION = P + "/collection/fixture"
SESSION = P + "/session/fixture"
ITEM = P + "/item/locked"
PROMPT = P + "/prompt/fixture"

XML = """<node>
<interface name="org.freedesktop.Secret.Service">
 <method name="OpenSession"><arg type="s" direction="in"/><arg type="v" direction="in"/><arg type="v" direction="out"/><arg type="o" direction="out"/></method>
 <method name="SearchItems"><arg type="a{ss}" direction="in"/><arg type="ao" direction="out"/><arg type="ao" direction="out"/></method>
 <method name="Unlock"><arg type="ao" direction="in"/><arg type="ao" direction="out"/><arg type="o" direction="out"/></method>
 <method name="ReadAlias"><arg type="s" direction="in"/><arg type="o" direction="out"/></method>
 <method name="GetSecrets"><arg type="ao" direction="in"/><arg type="o" direction="in"/><arg type="a{o(oayays)}" direction="out"/></method>
 <property name="Collections" type="ao" access="read"/>
</interface>
<interface name="org.freedesktop.Secret.Collection">
 <method name="CreateItem"><arg type="a{sv}" direction="in"/><arg type="(oayays)" direction="in"/><arg type="b" direction="in"/><arg type="o" direction="out"/><arg type="o" direction="out"/></method>
 <property name="Items" type="ao" access="read"/><property name="Label" type="s" access="read"/><property name="Locked" type="b" access="read"/><property name="Created" type="t" access="read"/><property name="Modified" type="t" access="read"/>
</interface>
<interface name="org.freedesktop.Secret.Item">
 <method name="Delete"><arg type="o" direction="out"/></method>
 <method name="GetSecret"><arg type="o" direction="in"/><arg type="(oayays)" direction="out"/></method>
 <property name="Locked" type="b" access="read"/><property name="Attributes" type="a{ss}" access="read"/><property name="Label" type="s" access="read"/><property name="Created" type="t" access="read"/><property name="Modified" type="t" access="read"/>
</interface>
<interface name="org.freedesktop.Secret.Session"><method name="Close"/></interface>
<interface name="org.freedesktop.Secret.Prompt">
 <method name="Prompt"><arg type="s" direction="in"/></method><method name="Dismiss"/>
 <signal name="Completed"><arg type="b"/><arg type="v"/></signal>
</interface></node>"""


class Service:
    def __init__(self, mode):
        self.mode = mode
        self.connection = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        self.paths = set()
        self.connection.add_filter(self.trace_method, None)
        self.items, self.pending, self.registrations = {}, [], []
        self.serial = 0
        self.delayed = False
        self.interfaces = {i.name: i for i in Gio.DBusNodeInfo.new_for_xml(XML).interfaces}
        self.register(P, "Service")
        self.register(COLLECTION, "Collection")
        # libsecret directly addresses the standard default alias when storing.
        self.register(P + "/aliases/default", "Collection")
        self.register(SESSION, "Session")
        self.register(PROMPT, "Prompt")
        if mode == "locked":
            self.items[ITEM] = ({"platform": "otakudobs-qa-private", "account": "fixture-account",
                                 "xdg:schema": "org.otakudobs.Multistream"}, b"fixture", "application/octet-stream")
            self.register(ITEM, "Item")
        reply = self.connection.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus",
                                         "org.freedesktop.DBus", "RequestName",
                                         GLib.Variant("(su)", ("org.freedesktop.secrets", 0)),
                                         GLib.VariantType.new("(u)"), Gio.DBusCallFlags.NONE, 1000, None)
        if reply.unpack()[0] != 1:
            raise RuntimeError("Private fixture could not acquire its own service name")

    def register(self, path, kind):
        self.paths.add(path)
        self.registrations.append(self.connection.register_object(
            path, self.interfaces["org.freedesktop.Secret." + kind], self.call, self.get_property, None))

    def trace_method(self, connection, message, incoming, data):
        if incoming and message.get_message_type() == Gio.DBusMessageType.METHOD_CALL:
            # Protocol names only; never print arguments, attributes or secrets.
            print("method", message.get_interface(), message.get_member(),
                  "registered=" + str(message.get_path() in self.paths), flush=True)
        return message

    def get_property(self, connection, sender, path, interface, name):
        if name in ("Created", "Modified"):
            return GLib.Variant("t", 1)
        if name == "Locked":
            return GLib.Variant("b", self.mode == "locked")
        if name == "Label":
            return GLib.Variant("s", "Private fixture")
        if name == "Collections":
            return GLib.Variant("ao", [COLLECTION])
        if name == "Items":
            return GLib.Variant("ao", list(self.items))
        if name == "Attributes":
            return GLib.Variant("a{ss}", self.items[path][0])
        raise RuntimeError("Unexpected fixture property " + name)

    def call(self, connection, sender, path, interface, method, parameters, invocation):
        args = parameters.unpack()
        if method == "OpenSession":
            if self.mode == "hang":
                self.pending.append(invocation)
                return
            if self.mode == "remote-error":
                invocation.return_dbus_error("org.freedesktop.DBus.Error.Failed",
                                             "OtakudOBS-QA-private-canary-v1")
            elif args[0] != "plain":
                invocation.return_dbus_error("org.freedesktop.DBus.Error.NotSupported",
                                             "Fixture supports plain session transport only")
            else:
                invocation.return_value(GLib.Variant("(vo)", (GLib.Variant("s", ""), SESSION)))
        elif method == "SearchItems":
            matches = [p for p, item in self.items.items()
                       if all(item[0].get(k) == v for k, v in args[0].items())]
            invocation.return_value(GLib.Variant("(aoao)", ([], matches) if self.mode == "locked" else (matches, [])))
        elif method == "ReadAlias":
            invocation.return_value(GLib.Variant("(o)", (COLLECTION,)))
        elif method == "Unlock":
            invocation.return_value(GLib.Variant("(aoo)", ([], PROMPT) if self.mode == "locked" else (args[0], "/")))
        elif method == "Prompt":
            invocation.return_value(GLib.Variant("()", ()))
            GLib.idle_add(self.dismiss)
        elif method == "Dismiss" or method == "Close":
            invocation.return_value(GLib.Variant("()", ()))
        elif method == "CreateItem":
            attrs = args[0]["org.freedesktop.Secret.Item.Attributes"]
            _, _, value, content_type = args[1]
            existing = next((p for p, item in self.items.items() if item[0] == attrs), None)
            if existing is None:
                self.serial += 1
                existing = P + "/item/n" + str(self.serial)
                self.register(existing, "Item")
            self.items[existing] = (attrs, bytes(value), content_type)
            response = GLib.Variant("(oo)", (existing, "/"))
            if self.mode in ("commit-cancel", "commit-timeout") and bytes(value).endswith(b"-v2") and not self.delayed:
                self.delayed = True
                self.pending.append(invocation)
                GLib.timeout_add(800, lambda: self.late_reply(invocation, response))
            else:
                invocation.return_value(response)
        elif method == "GetSecrets":
            secrets = {p: (args[1], [], list(self.items[p][1]), self.items[p][2])
                       for p in args[0] if p in self.items and self.mode != "locked"}
            invocation.return_value(GLib.Variant("(a{o(oayays)})", (secrets,)))
        elif method == "GetSecret":
            item = self.items[path]
            invocation.return_value(GLib.Variant("((oayays))", ((args[0], [], list(item[1]), item[2]),)))
        elif method == "Delete":
            self.items.pop(path, None)
            invocation.return_value(GLib.Variant("(o)", ("/",)))
        else:
            invocation.return_dbus_error("org.freedesktop.DBus.Error.UnknownMethod", "Unexpected fixture method")

    def dismiss(self):
        self.connection.emit_signal(None, PROMPT, "org.freedesktop.Secret.Prompt", "Completed",
                                    GLib.Variant("(bv)", (True, GLib.Variant("ao", []))))
        return False

    def late_reply(self, invocation, response):
        invocation.return_value(response)
        self.pending.remove(invocation)
        return False


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", required=True, choices=["hang", "locked", "remote-error", "commit-cancel", "commit-timeout"])
    parser.add_argument("--ready", required=True, type=Path)
    args = parser.parse_args()
    if os.environ.get("OTAKUDOBS_PRIVATE_BUS_ADDRESS") != os.environ.get("DBUS_SESSION_BUS_ADDRESS"):
        raise SystemExit("Private bus guard is required")
    service = Service(args.mode)
    args.ready.write_text(args.mode)
    GLib.MainLoop().run()


if __name__ == "__main__":
    main()
