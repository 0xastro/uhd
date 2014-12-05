//
// Copyright 2014 Ettus Research LLC
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include <uhd/usrp/rfnoc/rx_block_ctrl_base.hpp>
#include <uhd/utils/msg.hpp>
#include <uhd/usrp/rfnoc/constants.hpp>

using namespace uhd;
using namespace uhd::rfnoc;

void rx_block_ctrl_base::issue_stream_cmd(
        const uhd::stream_cmd_t &stream_cmd
) {
    UHD_RFNOC_BLOCK_TRACE() << "rx_block_ctrl_base::issue_stream_cmd()" << std::endl;
    if (_upstream_nodes.empty()) {
        UHD_MSG(warning) << "issue_stream_cmd() not implemented for " << get_block_id() << std::endl;
        return;
    }

    BOOST_FOREACH(const node_ctrl_base::node_map_pair_t upstream_node, _upstream_nodes) {
        source_node_ctrl::sptr this_upstream_block_ctrl =
            boost::dynamic_pointer_cast<source_node_ctrl>(upstream_node.second.lock());
        this_upstream_block_ctrl->issue_stream_cmd(stream_cmd);
    }
}

// non-virtual
void rx_block_ctrl_base::setup_rx_streamer(uhd::stream_args_t &args)
{
    UHD_RFNOC_BLOCK_TRACE() << "rx_block_ctrl_base::setup_rx_streamer()" << std::endl;

    // 0. Check if args collides with our own _args
    // and merge them
    BOOST_FOREACH(const std::string key, _args.keys()) {
        if (args.args.has_key(key) and _args[key] != args.args[key]) {
            throw uhd::runtime_error(
                    str(boost::format(
                            "Conflicting options for block %s: Block options require '%s' == '%s',\n"
                            "but streamer requests '%s' == '%s'."
                            ) % get_block_id().get() % key % _args[key] % key % args.args[key]
                    )
            );
        }
        args.args[key] = _args[key];
    }

    // 1. Call our own init_rx() function
    // This should modify "args" if necessary.
    _init_rx(args);

    // 2. Check if we're the last block
    if (_is_final_rx_block()) {
        UHD_MSG(status) << "rx_block_ctrl_base::setup_rx_streamer(): Final block, returning. " << std::endl;
        return;
    }

    // 3. Call all upstream blocks
    BOOST_FOREACH(const node_ctrl_base::node_map_pair_t upstream_node, _upstream_nodes) {
        // Make a copy so that modifications upstream aren't propagated downstream
        uhd::stream_args_t new_args = args;
        sptr this_upstream_block_ctrl =
            boost::dynamic_pointer_cast<rx_block_ctrl_base>(upstream_node.second.lock());
        if (this_upstream_block_ctrl) {
            this_upstream_block_ctrl->setup_rx_streamer(new_args);
        }
    }
}

size_t rx_block_ctrl_base::_request_output_port(
        const size_t suggested_port,
        const uhd::device_addr_t &args
) const {
    size_t port = source_node_ctrl::_request_output_port(suggested_port, args);
    if (not _tree->exists(_root_path / "output_sig" / port)) {
        return ANY_PORT;
    }
    return port;
}
// vim: sw=4 et:
