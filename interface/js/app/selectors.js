define(["jquery", "app/rspamd"],
    ($, rspamd) => {
        "use strict";
        const ui = {};

        function enable_disable_check_btn() {
            $("#selectorsChkMsgBtn").prop("disabled", (
                $.trim($("#selectorsMsgArea").val()).length === 0 ||
                !$("#selectorsSelArea").hasClass("is-valid")
            ));
        }

        function get_server() {
            const checked_server = rspamd.getSelector("selSrv");
            return (checked_server === "All SERVERS") ? "local" : checked_server;
        }

        function checkMsg(data) {
            const selector = $("#selectorsSelArea").val();
            rspamd.query("plugins/selectors/check_message?selector=" + encodeURIComponent(selector), {
                data: data,
                method: "POST",
                success: function (neighbours_status) {
                    const json = neighbours_status[0].data;
                    if (json.success) {
                        rspamd.alertMessage("alert-success", "Message successfully processed");
                        $("#selectorsResArea")
                            .val(Object.prototype.hasOwnProperty.call(json, "data") ? json.data.toString() : "");
                    } else {
                        rspamd.alertMessage("alert-error", "Unexpected error processing message");
                    }
                },
                server: get_server()
            });
        }

        function checkSelectors() {
            function toggle_form_group_class(remove, add) {
                $("#selectorsSelArea").removeClass("is-" + remove).addClass("is-" + add);
                enable_disable_check_btn();
            }
            const selector = $("#selectorsSelArea").val();
            if (selector.length && !rspamd.read_only) {
                rspamd.query("plugins/selectors/check_selector?selector=" + encodeURIComponent(selector), {
                    method: "GET",
                    success: function (json) {
                        if (json[0].data.success) {
                            toggle_form_group_class("invalid", "valid");
                        } else {
                            toggle_form_group_class("valid", "invalid");
                        }
                    },
                    server: get_server()
                });
            } else {
                $("#selectorsSelArea").removeClass("is-valid is-invalid");
                enable_disable_check_btn();
            }
        }

        function buildLists() {
            function build_table_from_json(json, table_id) {
                Object.keys(json).forEach((key) => {
                    const td = $("<td/>");
                    const tr = $("<tr/>")
                        .append(td.clone().html("<code>" + key + "</code>"))
                        .append(td.clone().html(json[key].description));
                    $(table_id + " tbody").append(tr);
                });
            }

            function getList(list) {
                rspamd.query("plugins/selectors/list_" + list, {
                    method: "GET",
                    success: function (neighbours_status) {
                        const json = neighbours_status[0].data;
                        build_table_from_json(json, "#selectorsTable-" + list);
                    },
                    server: get_server()
                });
            }

            getList("extractors");
            getList("transforms");
        }

        ui.displayUI = function () {
            if (!rspamd.read_only &&
                !$("#selectorsTable-extractors>tbody>tr").length &&
                !$("#selectorsTable-transforms>tbody>tr").length) buildLists();
            if (!$("#selectorsSelArea").is(".is-valid, .is-invalid")) checkSelectors();
        };


        function toggleSidebar(side) {
            $("#sidebar-" + side).toggleClass("collapsed");
            let contentClass = "col-lg-6";
            const openSidebarsCount = $("#sidebar-left").hasClass("collapsed") +
                    $("#sidebar-right").hasClass("collapsed");
            switch (openSidebarsCount) {
                case 1:
                    contentClass = "col-lg-9";
                    break;
                case 2:
                    contentClass = "col-lg-12";
                    break;
                default:
            }
            $("#content").removeClass("col-lg-12 col-lg-9 col-lg-6")
                .addClass(contentClass);
        }
        $("#sidebar-tab-left>a").click(() => {
            toggleSidebar("left");
            return false;
        });
        $("#sidebar-tab-right>a").click(() => {
            toggleSidebar("right");
            return false;
        });

        $("#selectorsMsgClean").on("click", () => {
            $("#selectorsChkMsgBtn").attr("disabled", true);
            $("#selectorsMsgArea").val("");
            return false;
        });
        $("#selectorsClean").on("click", () => {
            $("#selectorsSelArea").val("");
            checkSelectors();
            return false;
        });
        $("#selectorsChkMsgBtn").on("click", () => {
            $("#selectorsResArea").val("");
            checkMsg($("#selectorsMsgArea").val());
            return false;
        });

        $("#selectorsMsgArea").on("input", () => {
            enable_disable_check_btn();
        });
        $("#selectorsSelArea").on("input", () => {
            checkSelectors();
        });

        return ui;
    });
