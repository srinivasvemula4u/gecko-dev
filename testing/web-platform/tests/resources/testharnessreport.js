/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

var props = {output:false,
             explicit_timeout: true};
if (window.opener && "timeout_multiplier" in window.opener) {
    props["timeout_multiplier"] = window.opener.timeout_multiplier;
}
setup(props);
add_completion_callback(function() {
    add_completion_callback(function(tests, status) {
        window.opener.done(tests, status)
    })
});
