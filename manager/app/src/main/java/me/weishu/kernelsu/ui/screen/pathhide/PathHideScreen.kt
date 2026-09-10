package me.weishu.kernelsu.ui.screen.pathhide

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.add
import androidx.compose.foundation.layout.displayCutout
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.systemBars
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.topjohnwu.superuser.Shell
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.weishu.kernelsu.ui.navigation3.LocalNavigator
import me.weishu.kernelsu.ui.theme.LocalEnableBlur
import me.weishu.kernelsu.ui.util.BlurredBar
import me.weishu.kernelsu.ui.util.rememberBlurBackdrop
import top.yukonga.miuix.kmp.basic.Card
import top.yukonga.miuix.kmp.basic.Icon
import top.yukonga.miuix.kmp.basic.IconButton
import top.yukonga.miuix.kmp.basic.MiuixScrollBehavior
import top.yukonga.miuix.kmp.basic.Scaffold
import top.yukonga.miuix.kmp.basic.SmallTopAppBar
import top.yukonga.miuix.kmp.basic.Text
import top.yukonga.miuix.kmp.preference.ArrowPreference
import top.yukonga.miuix.kmp.blur.layerBackdrop
import top.yukonga.miuix.kmp.icon.MiuixIcons
import top.yukonga.miuix.kmp.icon.extended.Back
import top.yukonga.miuix.kmp.theme.MiuixTheme.colorScheme
import top.yukonga.miuix.kmp.utils.overScrollVertical

@Composable
fun PathHideScreen() {
    val navigator = LocalNavigator.current
    val scope = rememberCoroutineScope()
    val scrollBehavior = MiuixScrollBehavior()
    val enableBlur = LocalEnableBlur.current
    val backdrop = rememberBlurBackdrop(enableBlur)

    var config by remember { mutableStateOf("") }
    var status by remember { mutableStateOf("") }
    var message by remember { mutableStateOf("") }

    suspend fun runKsu(cmd: String): String = withContext(Dispatchers.IO) {
        val result = Shell.cmd(cmd).exec()
        if (result.isSuccess) result.out.joinToString("\n") else result.err.joinToString("\n")
    }

    suspend fun refresh() = withContext(Dispatchers.IO) {
        config = runKsu("ksud path-hide get-config")
        status = runKsu("ksud path-hide status")
    }

    LaunchedEffect(Unit) { refresh() }

    Scaffold(
        topBar = {
            BlurredBar(backdrop) {
                SmallTopAppBar(
                    title = "路径隐藏",
                    scrollBehavior = scrollBehavior,
                    navigationIcon = {
                        IconButton(onClick = { navigator.pop() }) {
                            Icon(
                                imageVector = MiuixIcons.Back,
                                contentDescription = "返回",
                                tint = colorScheme.onBackground
                            )
                        }
                    }
                )
            }
        },
        popupHost = { },
        contentWindowInsets = WindowInsets.systemBars.add(WindowInsets.displayCutout).only(WindowInsetsSides.Horizontal),
    ) { innerPadding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
                .verticalScroll(rememberScrollState())
                .overScrollVertical()
                .padding(horizontal = 12.dp)
        ) {
            Spacer(Modifier.height(12.dp))

            Card(modifier = Modifier.fillMaxWidth()) {
                Column(modifier = Modifier.padding(16.dp)) {
                    Text(text = "内核级路径隐藏", color = colorScheme.onSurface)
                    Spacer(Modifier.height(4.dp))
                    Text(
                        text = "每行一个绝对路径，以 / 开头。# 开头为注释。保存后自动重载内核模块。",
                        color = colorScheme.onSurface.copy(alpha = 0.7f)
                    )
                }
            }

            Spacer(Modifier.height(12.dp))

            Card(modifier = Modifier.fillMaxWidth()) {
                Column(modifier = Modifier.padding(16.dp)) {
                    Text(text = "隐藏路径列表", color = colorScheme.onSurface)
                    Spacer(Modifier.height(8.dp))
                    BasicTextField(
                        value = config,
                        onValueChange = { config = it },
                        textStyle = TextStyle(
                            color = colorScheme.onSurface,
                            fontSize = 14.sp,
                        ),
                        modifier = Modifier
                            .fillMaxWidth()
                            .heightIn(min = 160.dp)
                            .background(
                                color = colorScheme.surface.copy(alpha = 0.3f),
                                shape = RoundedCornerShape(12.dp)
                            )
                            .padding(12.dp)
                    )
                }
            }

            Spacer(Modifier.height(16.dp))

            Card(modifier = Modifier.fillMaxWidth()) {
                ArrowPreference(
                    title = "保存并重载",
                    summary = "写入配置并重新加载内核模块",
                    onClick = {
                        scope.launch {
                            runKsu("printf '%s' '$config' | ksud path-hide set-config")
                            runKsu("ksud path-hide reload")
                            message = "已保存并重载"
                            refresh()
                        }
                    }
                )
                ArrowPreference(
                    title = "加载模块",
                    summary = "insmod pathhide.ko",
                    onClick = {
                        scope.launch {
                            runKsu("ksud path-hide load")
                            message = "已加载"
                            refresh()
                        }
                    }
                )
                ArrowPreference(
                    title = "卸载模块",
                    summary = "rmmod pathhide",
                    onClick = {
                        scope.launch {
                            runKsu("ksud path-hide unload")
                            message = "已卸载"
                            refresh()
                        }
                    }
                )
            }

            if (message.isNotEmpty()) {
                Spacer(Modifier.height(16.dp))
                Card(modifier = Modifier.fillMaxWidth()) {
                    Text(
                        text = message,
                        color = colorScheme.onSurface,
                        modifier = Modifier.padding(16.dp)
                    )
                }
            }

            Spacer(Modifier.height(16.dp))

            Card(modifier = Modifier.fillMaxWidth()) {
                Column(modifier = Modifier.padding(16.dp)) {
                    Text(text = "状态", color = colorScheme.onSurface)
                    Spacer(Modifier.height(8.dp))
                    Text(
                        text = status.ifEmpty { "加载中..." },
                        color = colorScheme.onSurface.copy(alpha = 0.7f)
                    )
                }
            }

            Spacer(Modifier.height(24.dp))
        }
    }
}
