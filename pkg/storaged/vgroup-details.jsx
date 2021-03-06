/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2017 Red Hat, Inc.
 *
 * Cockpit is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * Cockpit is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Cockpit; If not, see <http://www.gnu.org/licenses/>.
 */

import cockpit from "cockpit";
import React from "react";

import { Card, CardBody, CardTitle, CardHeader, CardActions, Text, TextVariants } from "@patternfly/react-core";

import * as utils from "./utils.js";
import { fmt_to_fragments } from "./utilsx.jsx";
import { StdDetailsLayout } from "./details.jsx";
import { SidePanel, SidePanelBlockRow } from "./side-panel.jsx";
import { VGroup } from "./content-views.jsx";
import { StorageButton } from "./storage-controls.jsx";
import {
    dialog_open, TextInput, SelectSpaces,
    BlockingMessage, TeardownMessage
} from "./dialog.jsx";

const _ = cockpit.gettext;

class VGroupSidebar extends React.Component {
    render() {
        var self = this;
        var client = self.props.client;
        var vgroup = self.props.vgroup;
        var pvols = client.vgroups_pvols[vgroup.path] || [];

        function filter_inside_vgroup(spc) {
            var block = spc.block;
            if (client.blocks_part[block.path])
                block = client.blocks[client.blocks_part[block.path].Table];
            var lvol = (block &&
                        client.blocks_lvm2[block.path] &&
                        client.lvols[client.blocks_lvm2[block.path].LogicalVolume]);
            return !lvol || lvol.VolumeGroup != vgroup.path;
        }

        function add_disk() {
            dialog_open({
                Title: _("Add Disks"),
                Fields: [
                    SelectSpaces("disks", _("Disks"),
                                 {
                                     empty_warning: _("No disks are available."),
                                     validate: function(disks) {
                                         if (disks.length === 0)
                                             return _("At least one disk is needed.");
                                     },
                                     spaces: utils.get_available_spaces(client).filter(filter_inside_vgroup)
                                 })
                ],
                Action: {
                    Title: _("Add"),
                    action: function(vals) {
                        return utils.prepare_available_spaces(client, vals.disks).then(paths =>
                            Promise.all(paths.map(p => vgroup.AddDevice(p, {}))));
                    }
                }
            });
        }

        function render_pvol(pvol) {
            var remove_action = null;
            var remove_excuse = null;

            function pvol_remove() {
                return vgroup.RemoveDevice(pvol.path, true, {});
            }

            function pvol_empty_and_remove() {
                return (vgroup.EmptyDevice(pvol.path, {})
                        .then(function() {
                            vgroup.RemoveDevice(pvol.path, true, {});
                        }));
            }

            if (pvols.length === 1) {
                remove_excuse = _("The last physical volume of a volume group cannot be removed.");
            } else if (pvol.FreeSize < pvol.Size) {
                if (pvol.Size <= vgroup.FreeSize)
                    remove_action = pvol_empty_and_remove;
                else
                    remove_excuse = cockpit.format(
                        _("There is not enough free space elsewhere to remove this physical volume. At least $0 more free space is needed."),
                        utils.fmt_size(pvol.Size - vgroup.FreeSize)
                    );
            } else {
                remove_action = pvol_remove;
            }

            return (
                <SidePanelBlockRow client={client}
                                    block={client.blocks[pvol.path]}
                                    detail={cockpit.format(_("$0, $1 free"),
                                                           utils.fmt_size(pvol.Size),
                                                           utils.fmt_size(pvol.FreeSize))}
                                    actions={<StorageButton onClick={remove_action} excuse={remove_excuse}>
                                        <span className="fa fa-minus" />
                                    </StorageButton>}
                                    key={pvol.path} />);
        }

        return (
            <SidePanel title={_("Physical Volumes")}
                       actions={<StorageButton onClick={add_disk}><span className="fa fa-plus" /></StorageButton>}>
                { pvols.map(render_pvol) }
            </SidePanel>
        );
    }
}

export class VGroupDetails extends React.Component {
    constructor() {
        super();
        this.poll_timer = null;
    }

    ensurePolling(needs_polling) {
        if (needs_polling && this.poll_timer === null) {
            this.poll_timer = window.setInterval(() => { this.props.vgroup.Poll() }, 2000);
        } else if (!needs_polling && this.poll_timer !== null) {
            window.clearInterval(this.poll_timer);
            this.poll_timer = null;
        }
    }

    componentWillUnmount() {
        this.ensurePolling(false);
    }

    render() {
        var client = this.props.client;
        var vgroup = this.props.vgroup;

        this.ensurePolling(vgroup.NeedsPolling);

        function rename() {
            var location = cockpit.location;

            dialog_open({
                Title: _("Rename Volume Group"),
                Fields: [
                    TextInput("name", _("Name"),
                              {
                                  value: vgroup.Name,
                                  validate: utils.validate_lvm2_name
                              })
                ],
                Action: {
                    Title: _("Rename"),
                    action: function (vals) {
                        return vgroup.Rename(vals.name, { })
                                .done(function () {
                                    location.go(['vg', vals.name]);
                                });
                    }
                }
            });
        }

        function delete_() {
            var location = cockpit.location;
            var usage = utils.get_active_usage(client, vgroup.path);

            if (usage.Blocking) {
                dialog_open({
                    Title: cockpit.format(_("$0 is in active use"),
                                          vgroup.Name),
                    Body: BlockingMessage(usage)
                });
                return;
            }

            dialog_open({
                Title: cockpit.format(_("Please confirm deletion of $0"), vgroup.Name),
                Footer: TeardownMessage(usage),
                Action: {
                    Danger: _("Deleting a volume group will erase all data on it."),
                    Title: _("Delete"),
                    action: function () {
                        return utils.teardown_active_usage(client, usage)
                                .then(function () {
                                    return vgroup.Delete(true,
                                                         { 'tear-down': { t: 'b', v: true } })
                                            .done(function () {
                                                location.go('/');
                                            });
                                });
                    }
                }
            });
        }

        var header = (
            <Card>
                <CardHeader>
                    <CardTitle><Text component={TextVariants.h2}>{fmt_to_fragments(_("Volume Group $0"), <b>{vgroup.Name}</b>)}</Text></CardTitle>
                    <CardActions>
                        <StorageButton onClick={rename}>{_("Rename")}</StorageButton>
                        { "\n" }
                        <StorageButton kind="danger" onClick={delete_}>{_("Delete")}</StorageButton>
                    </CardActions>
                </CardHeader>
                <CardBody>
                    <div className="ct-form">
                        <label className="control-label">{_("storage", "UUID")}</label>
                        <div>{ vgroup.UUID }</div>

                        <label className="control-label">{_("storage", "Capacity")}</label>
                        <div>{ utils.fmt_size_long(vgroup.Size) }</div>
                    </div>
                </CardBody>
            </Card>
        );

        var sidebar = <VGroupSidebar client={this.props.client} vgroup={vgroup} />;

        var content = <VGroup client={this.props.client} vgroup={vgroup} />;

        return <StdDetailsLayout client={this.props.client}
                                 header={ header }
                                 sidebar={ sidebar }
                                 content={ content } />;
    }
}
